/*
 *  Win32 VBI capture driver management
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation. You find a copy of this
 *  license in the file COPYRIGHT in the root directory of this release.
 *
 *  THIS PROGRAM IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL,
 *  BUT WITHOUT ANY WARRANTY; WITHOUT EVEN THE IMPLIED WARRANTY OF
 *  MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *
 *  Description:
 *
 *    This module manages M$ Windows drivers for all supported TV cards.
 *    It provides an abstraction layer with higher-level functions,
 *    e.g. to start/stop acquisition or change the channel.
 *
 *    It's based on DSdrv by Mathias Ellinger and John Adcock: a free open-source
 *    driver for PCI cards. The driver offers generic I/O functions to directly
 *    access the hardware (memory map PCI registers, allocate memory for DMA
 *    etc.)  Much of the code to control the specific cards was also taken from
 *    DScaler, although a large part (if not the most) originally came from the
 *    Linux bttv and saa7134 drivers (see also copyrights below).  Linux bttv
 *    was originally ported to Windows by "Espresso".
 *
 *
 *  Authors:
 *
 *    WinDriver adaption (from MultiDec)
 *      Copyright (C) 2000 Espresso (echter_espresso@hotmail.com)
 *
 *    WinDriver replaced with DSdrv Bt8x8 code (DScaler driver)
 *      March 2002 by E-Nek (e-nek@netcourrier.com)
 *
 *    WDM support
 *      February 2004 by G�rard Chevalier (gd_chevalier@hotmail.com)
 *
 *    The rest
 *      Tom Zoerner
 *
 *
 *  $Id: btdrv4win.c,v 1.60 2007/12/31 16:34:13 tom Exp tom $
 */

#define DEBUG_SWITCH DEBUG_SWITCH_VBI
#define DPRINTF_OFF

#include <windows.h>
#include <aclapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mytypes.h"
#include "debug.h"

#include "btdrv.h"
//#include "epgvbi/winshm.h"
//#include "epgvbi/winshmsrv.h"

//#include "wdmdrv/wdmdrv.h"
//#include "epgvbi/vbidecode.h"
//#include "epgvbi/zvbidecoder.h"
//#include "epgvbi/tvchan.h"
//#include "epgvbi/syserrmsg.h"

#include "dsdrvlib.h"
#include "tvcard.h"
#include "bt8x8.h"
//#include "dsdrv/saa7134.h"
//#include "dsdrv/cx2388x.h"
//#include "dsdrv/wintuner.h"


// ----------------------------------------------------------------------------
// State of PCI cards

typedef struct
{
   WORD   VendorId;
   WORD   DeviceId;
   char * szName;
} TCaptureChip;

#define PCI_ID_BROOKTREE  0x109e
#define PCI_ID_PHILIPS    0x1131
#define PCI_ID_CONEXANT   0x14F1

static const TCaptureChip CaptureChips[] =
{
   { PCI_ID_BROOKTREE, 0x036e, "Brooktree Bt878"  },
   { PCI_ID_BROOKTREE, 0x036f, "Brooktree Bt878A" },
   { PCI_ID_BROOKTREE, 0x0350, "Brooktree Bt848"  },
   { PCI_ID_BROOKTREE, 0x0351, "Brooktree Bt849"  },
   { PCI_ID_PHILIPS,   0x7134, "Philips SAA7134" },
   { PCI_ID_PHILIPS,   0x7133, "Philips SAA7133" },
   { PCI_ID_PHILIPS,   0x7130, "Philips SAA7130" },
   //{ PCI_ID_PHILIPS,  0x7135, "Philips SAA7135" },
   { PCI_ID_CONEXANT,  0x8800, "Conexant CX23881 (Bt881)" }
};
#define CAPTURE_CHIP_COUNT (sizeof(CaptureChips) / sizeof(CaptureChips[0]))

typedef struct
{
   uint  chipIdx;
   uint  chipCardIdx;
   WORD  VendorId;
   WORD  DeviceId;
   DWORD dwSubSystemID;
   DWORD dwBusNumber;
   DWORD dwSlotNumber;
} PCI_SOURCE;

// ----------------------------------------------------------------------------
// Shared variables between PCI and WDM sources

#define MAX_CARD_COUNT  4
#define CARD_COUNT_UNINITIALIZED  (~0u)
static PCI_SOURCE     btCardList[MAX_CARD_COUNT];
static uint           btCardCount;

#define INVALID_INPUT_SOURCE  0xff

static struct
{
   BTDRV_SOURCE_TYPE drvType;
   uint        sourceIdx;
   uint        cardId;
   uint        tunerType;
   uint        pllType;
   uint        wdmStop;
   uint        threadPrio;
   uint        inputSrc;
   uint        tunerFreq;
   uint        tunerNorm;
} btCfg;

static TVCARD cardif;
static BOOL pciDrvLoaded;
static BOOL wdmDrvLoaded;
static BOOL shmSlaveMode = FALSE;

// ----------------------------------------------------------------------------

volatile EPGACQ_BUF * pVbiBuf;
static EPGACQ_BUF vbiBuf;

static bool BtDriver_DsDrvCountSources( bool showDrvErr );
static BOOL BtDriver_PciCardOpen( uint sourceIdx );

// ----------------------------------------------------------------------------
// Helper function to set user-configured priority in IRQ and VBI threads
//
static int BtDriver_GetAcqPriority( int level )
{
   int prio;

   switch (level)
   {
      default:
      case 0: prio = THREAD_PRIORITY_NORMAL; break;
      case 1: prio = THREAD_PRIORITY_ABOVE_NORMAL; break;
      // skipping HIGHEST by (arbitrary) choice
      case 2: prio = THREAD_PRIORITY_TIME_CRITICAL; break;
   }

   return prio;
}

// ---------------------------------------------------------------------------
// Get interface functions for a TV card
//
static bool BtDriver_PciCardGetInterface( TVCARD * pTvCard, DWORD VendorId, uint sourceIdx )
{
   bool result = TRUE;

   memset(pTvCard, 0, sizeof(* pTvCard));

   switch (VendorId)
   {
      case PCI_ID_BROOKTREE:
         Bt8x8_GetInterface(pTvCard);
         break;

      default:
         result = FALSE;
         break;
   }

   // copy card parameters into the struct
   if ((btCardCount != CARD_COUNT_UNINITIALIZED) && (sourceIdx < btCardCount))
   {
      assert(btCardList[sourceIdx].VendorId == VendorId);

      pTvCard->params.BusNumber   = btCardList[sourceIdx].dwBusNumber;
      pTvCard->params.SlotNumber  = btCardList[sourceIdx].dwSlotNumber;
      pTvCard->params.VendorId    = btCardList[sourceIdx].VendorId;
      pTvCard->params.DeviceId    = btCardList[sourceIdx].DeviceId;
      pTvCard->params.SubSystemId = btCardList[sourceIdx].dwSubSystemID;
   }

   return result;
}

// ---------------------------------------------------------------------------
// Free resources allocated in card drivers
//
static void BtDriver_PciCardsRelease( void )
{
   TVCARD tmpCardIf;

   Bt8x8_GetInterface(&tmpCardIf);
   tmpCardIf.cfg->FreeCardList();
}

// ---------------------------------------------------------------------------
// Select the video input source
// - which input is tuner and which composite etc. is completely up to the
//   card manufacturer, but it seems that almost all use the 2,3,1,1 muxing
// - returns TRUE in *pIsTuner if the selected source is the TV tuner
//
bool BtDriver_SetInputSource( uint inputIdx, uint norm, bool * pIsTuner )
{
   bool isTuner = FALSE;
   bool result = FALSE;
   HRESULT hres;

   // remember the input source for later
   btCfg.inputSrc = inputIdx;

    if (pciDrvLoaded)
    {
        // XXX TODO norm switch
        result = cardif.cfg->SetVideoSource(&cardif, inputIdx);

        isTuner = cardif.cfg->IsInputATuner(&cardif, inputIdx);
    }

   if (pIsTuner != NULL)
      *pIsTuner = isTuner;

   return result;
}

// ---------------------------------------------------------------------------
// Return name for given input source index
// - has to be called repeatedly with incremented indices until NULL is returned
//
const char * BtDriver_GetInputName( uint sourceIdx, uint cardType, uint drvType, uint inputIdx )
{
   const char * pName = NULL;
   TVCARD tmpCardIf;
   uint   chipIdx;
   HRESULT hres;

   if (drvType == BTDRV_SOURCE_PCI)
   {
      if ((btCardCount != CARD_COUNT_UNINITIALIZED) && (sourceIdx < btCardCount))
      {
         chipIdx = btCardList[sourceIdx].chipIdx;

         if (BtDriver_PciCardGetInterface(&tmpCardIf, CaptureChips[chipIdx].VendorId, CARD_COUNT_UNINITIALIZED) )
         {
            tmpCardIf.params.cardId = cardType;

            if (inputIdx < tmpCardIf.cfg->GetNumInputs(&tmpCardIf))
               pName = tmpCardIf.cfg->GetInputName(&tmpCardIf, inputIdx);
            else
               pName = NULL;
         }
      }
      dprintf4("BtDriver-GetInputName: PCI sourceIdx=%d, cardType=%d, inputIdx=%d: result=%s\n", sourceIdx, cardType, inputIdx, ((pName != NULL) ? pName : "NULL"));
   }
   else
      debug4("BtDriver-GetInputName: invalid driver type %d: sourceIdx=%d, cardType=%d, inputIdx=%d", drvType, sourceIdx, cardType, inputIdx);

   return pName;
}

// ---------------------------------------------------------------------------
// Auto-detect card type and return parameters from config table
// - card type may also be set manually, in which case only params are returned
//
bool BtDriver_QueryCardParams( uint sourceIdx, sint * pCardType, sint * pTunerType, sint * pPllType )
{
   TVCARD tmpCardIf;
   uint   chipIdx;
   bool   drvWasLoaded;
   uint   loadError;
   bool   result = FALSE;

   if ((pCardType != NULL) && (pTunerType != NULL) && (pPllType != NULL))
   {
      if ((btCardCount != CARD_COUNT_UNINITIALIZED) && (sourceIdx < btCardCount))
      {
            if (pciDrvLoaded && (btCfg.sourceIdx != sourceIdx))
         {  // error
            debug2("BtDriver-QueryCardParams: acq running for different card %d instead req. %d", btCfg.sourceIdx, sourceIdx);
            MessageBox(NULL, L"Acquisition is running for a different TV card.\nStop acquisition and try again.", L"nxtvepg driver problem", MB_ICONSTOP | MB_OK | MB_TASKMODAL | MB_SETFOREGROUND);
         }
         else
         {
            drvWasLoaded = pciDrvLoaded;
            if (pciDrvLoaded == FALSE)
            {
                  loadError = DsDrvLoad();
                  if (loadError == HWDRV_LOAD_SUCCESS)
                  {
                     pciDrvLoaded = TRUE;

                     // scan the PCI bus for known cards
                     BtDriver_DsDrvCountSources(TRUE);

                     if (BtDriver_PciCardOpen(sourceIdx) == FALSE)
                     {
                        DsDrvUnload();
                        pciDrvLoaded = FALSE;
                     }
                  }
                  else
                  {
                     MessageBox(NULL, DsDrvGetErrorMsg(loadError), L"nxtvepg driver problem", MB_ICONSTOP | MB_OK | MB_TASKMODAL | MB_SETFOREGROUND);
                  }
            }

            if (pciDrvLoaded)
            {
               chipIdx = btCardList[sourceIdx].chipIdx;
               if ( BtDriver_PciCardGetInterface(&tmpCardIf, CaptureChips[chipIdx].VendorId, sourceIdx) )
               {
                  if (*pCardType <= 0)
                     *pCardType  = tmpCardIf.cfg->AutoDetectCardType(&tmpCardIf);

                  if (*pCardType > 0)
                  {
                     *pPllType   = tmpCardIf.cfg->GetPllType(&tmpCardIf, *pCardType);
                  }
                  else
                  {
                     *pTunerType = *pPllType = 0;
                  }
                  result = TRUE;
               }

               if (drvWasLoaded == FALSE)
               {
                  DsDrvUnload();
                  pciDrvLoaded = FALSE;
               }
            }
         }
      }
      else
         debug2("BtDriver-QueryCardParams: PCI bus not scanned or invalid card idx %d >= count %d", sourceIdx, btCardCount);
   }
   else
      fatal3("BtDriver-QueryCardParams: illegal NULL ptr params %lx,%lx,%lx", (long)pCardType, (long)pTunerType, (long)pPllType);

   return result;
}

// ---------------------------------------------------------------------------
// Return name from card list for a given chip
// - only used for PCI sources to select card manufacturer and model
//
const char * BtDriver_GetCardNameFromList( uint sourceIdx, uint listIdx )
{
   const char * pName = NULL;
   TVCARD tmpCardIf;
   uint chipIdx;

   if ((btCardCount != CARD_COUNT_UNINITIALIZED) && (sourceIdx < btCardCount))
   {
      chipIdx = btCardList[sourceIdx].chipIdx;

      if ( BtDriver_PciCardGetInterface(&tmpCardIf, CaptureChips[chipIdx].VendorId, CARD_COUNT_UNINITIALIZED) )
      {
         pName = tmpCardIf.cfg->GetCardName(&tmpCardIf, listIdx);
      }
   }
   return pName;
}

// ---------------------------------------------------------------------------
// Determine default driver type upon first start
// - prefer WDM if DLL is available
//
BTDRV_SOURCE_TYPE BtDriver_GetDefaultDrvType( void )
{
   return BTDRV_SOURCE_PCI;
}

// ---------------------------------------------------------------------------
// Return name & chip type for given TV card
// - if the driver was never loaded before the PCI bus is scanned now;
//   no error message displayed if the driver fails to load, but result set to FALSE
// - end of enumeration is indicated by a FALSE result or NULL name pointer
//
bool BtDriver_EnumCards( uint drvType, uint sourceIdx, uint cardType,
                         uint * pChipType, const char ** pName, bool showDrvErr )
{
   static char cardName[50];
   TVCARD tmpCardIf;
   uint   chipIdx;
   uint   chipType;
   HRESULT hres;
   bool   result = FALSE;

   if ((pChipType != NULL) && (pName != NULL))
   {
      if (drvType == BTDRV_SOURCE_PCI)
      {
         // note: only try to load driver for the first query
         if ( (sourceIdx == 0) && (btCardCount == CARD_COUNT_UNINITIALIZED) )
         {
            assert(pciDrvLoaded == FALSE);
            BtDriver_DsDrvCountSources(showDrvErr);
         }

         if (btCardCount != CARD_COUNT_UNINITIALIZED)
         {
            if (sourceIdx < btCardCount)
            {
               chipIdx  = btCardList[sourceIdx].chipIdx;
               chipType = (CaptureChips[chipIdx].VendorId << 16) | CaptureChips[chipIdx].DeviceId;

               if ((cardType != 0) && (chipType == *pChipType))
               {
                  if ( BtDriver_PciCardGetInterface(&tmpCardIf, CaptureChips[chipIdx].VendorId, CARD_COUNT_UNINITIALIZED) )
                  {
                     *pName = tmpCardIf.cfg->GetCardName(&tmpCardIf, cardType);
                  }
                  else
                     *pName = "unknown chip type";
               }
               else
               {
                  sprintf(cardName, "unknown %s card", CaptureChips[chipIdx].szName);
                  *pName = cardName;
               }
               *pChipType = chipType;
            }
            else
               *pName = NULL;
         }
         else
         {  // failed to load driver for PCI scan -> just return names for already known cards
            if (*pChipType != 0)
            {
               if ( BtDriver_PciCardGetInterface(&tmpCardIf, *pChipType >> 16, CARD_COUNT_UNINITIALIZED) )
               {
                  *pName = tmpCardIf.cfg->GetCardName(&tmpCardIf, cardType);
               }
               else
                  *pName = "unknown card";

               // return 0 as chip type to indicate driver failure
               *pChipType = 0;
            }
            else
            {  // end of enumeration
               if (shmSlaveMode && showDrvErr && (sourceIdx == 0))
               {
                  MessageBox(NULL, L"Cannot scan PCI bus for TV cards while connected to a TV application.\nTerminated the TV application and try again.", L"nxtvepg driver problem", MB_ICONSTOP | MB_OK | MB_TASKMODAL | MB_SETFOREGROUND);
               }
               *pName = NULL;
            }
         }
         result = TRUE;
      }
      else
      {  // end of enumeration
         debug1("BtDriver-EnumCards: unknown driver tpe: %d", drvType);
         *pName = NULL;
      }
   }
   else
      fatal2("BtDriver-EnumCards: illegal NULL ptr params %lx,%lx", (long)pChipType, (long)pName);

   return result;
}

// ---------------------------------------------------------------------------
// Check if the current source has a video signal
//
bool BtDriver_IsVideoPresent( void )
{
   HRESULT wdmResult;
   bool result;

   if (pciDrvLoaded)
      result = cardif.ctl->IsVideoPresent();
   else
      result = FALSE;

   return result;
}

// ---------------------------------------------------------------------------
// Scan PCI Bus for known TV card capture chips
//
static void BtDriver_PciCardCount( void )
{
   uint  chipIdx, cardIdx;

   btCardCount = 0;
   for (chipIdx=0; (chipIdx < CAPTURE_CHIP_COUNT) && (btCardCount < MAX_CARD_COUNT); chipIdx++)
   {
      cardIdx = 0;
      while (btCardCount < MAX_CARD_COUNT)
      {
         if (DoesThisPCICardExist(CaptureChips[chipIdx].VendorId, CaptureChips[chipIdx].DeviceId,
                                  cardIdx,
                                  &btCardList[btCardCount].dwSubSystemID,
                                  &btCardList[btCardCount].dwBusNumber,
                                  &btCardList[btCardCount].dwSlotNumber) == ERROR_SUCCESS)
         {
            dprintf4("PCI scan: found capture chip %s, ID=%lx, bus=%ld, slot=%ld\n", CaptureChips[chipIdx].szName, btCardList[btCardCount].dwSubSystemID, btCardList[btCardCount].dwBusNumber, btCardList[btCardCount].dwSlotNumber);

            btCardList[btCardCount].VendorId    = CaptureChips[chipIdx].VendorId;
            btCardList[btCardCount].DeviceId    = CaptureChips[chipIdx].DeviceId;
            btCardList[btCardCount].chipIdx     = chipIdx;
            btCardList[btCardCount].chipCardIdx = cardIdx;
            btCardCount += 1;
         }
         else
         {  // no more cards with this chip -> next chip (outer loop)
            break;
         }
         cardIdx += 1;
      }
   }
   dprintf1("BtDriver-PciCardCount: found %d PCI cards\n", btCardCount);
}

// ---------------------------------------------------------------------------
// Generate a list of available cards
//
static bool BtDriver_DsDrvCountSources( bool showDrvErr )
{
   uint  loadError;
   bool  result = FALSE;

   // if the scan was already done skip it
   if (btCardCount == CARD_COUNT_UNINITIALIZED)
   {
      dprintf0("BtDriver-CountSources: PCI start\n");

      // note: don't reserve the card from TV app since a PCI scan does not cause conflicts
      if (pciDrvLoaded == FALSE)
      {
         loadError = DsDrvLoad();
         if (loadError == HWDRV_LOAD_SUCCESS)
         {
            // scan the PCI bus for known cards, but don't open any
            BtDriver_PciCardCount();
            DsDrvUnload();
            result = TRUE;
         }
         else if (showDrvErr)
         {
            MessageBox(NULL, DsDrvGetErrorMsg(loadError), L"nxtvepg driver problem", MB_ICONSTOP | MB_OK | MB_TASKMODAL | MB_SETFOREGROUND);
         }
      }
      else
      {  // dsdrv already loaded -> just do the scan
         BtDriver_PciCardCount();
         result = TRUE;
      }
   }
   else
      result = TRUE;

   return result;
}

// ---------------------------------------------------------------------------
// Open the driver device and allocate I/O resources
//
static BOOL BtDriver_PciCardOpen( uint sourceIdx )
{
   WCHAR msgbuf[200];
   int  ret;
   int  chipIdx, chipCardIdx;
   BOOL supportsAcpi;
   BOOL result = FALSE;

   if (sourceIdx < btCardCount)
   {
      chipIdx     = btCardList[sourceIdx].chipIdx;
      chipCardIdx = btCardList[sourceIdx].chipCardIdx;

      if ( BtDriver_PciCardGetInterface(&cardif, CaptureChips[chipIdx].VendorId, sourceIdx) )
      {
         supportsAcpi = cardif.cfg->SupportsAcpi(&cardif);

         ret = pciGetHardwareResources(CaptureChips[chipIdx].VendorId,
                                       CaptureChips[chipIdx].DeviceId,
                                       chipCardIdx,
                                       supportsAcpi,
                                       cardif.ctl->ResetChip);

         if (ret == ERROR_SUCCESS)
         {
            dprintf2("BtDriver-PciCardOpen: %s driver loaded, card #%d opened\n", CaptureChips[chipIdx].szName, sourceIdx);
            result = TRUE;
         }
         else if (ret == 3)
         {  // card found, but failed to open -> abort
            wsprintf(msgbuf, L"Capture card #%d (with %s chip) cannot be locked!", sourceIdx, CaptureChips[chipIdx].szName);
            MessageBox(NULL, msgbuf, L"nxtvepg driver problem", MB_ICONSTOP | MB_OK | MB_TASKMODAL | MB_SETFOREGROUND);
         }
      }
   }
   return result;
}

// ----------------------------------------------------------------------------
// Shut down the driver and free all resources
// - after this function is called, other processes can use the card
//
static void BtDriver_DsDrvUnload( void )
{
   if (pciDrvLoaded)
   {
      cardif.ctl->Close(&cardif);
      DsDrvUnload();

      // clear driver interface functions (debugging only)
      memset(&cardif.ctl, 0, sizeof(cardif.ctl));
      memset(&cardif.cfg, 0, sizeof(cardif.cfg));

      pciDrvLoaded = FALSE;
   }
   dprintf0("BtDriver-DsDrvUnload: driver unloaded\n");
}

// ----------------------------------------------------------------------------
// Boot the dsdrv driver, allocate resources and initialize all subsystems
//
static bool BtDriver_DsDrvLoad( void )
{
   DWORD loadError;
   bool result = FALSE;

   assert((shmSlaveMode == FALSE) && (wdmDrvLoaded == FALSE));

   loadError = DsDrvLoad();
   if (loadError == HWDRV_LOAD_SUCCESS)
   {
      pciDrvLoaded = TRUE;
      BtDriver_DsDrvCountSources(FALSE);

      if (btCfg.sourceIdx < btCardCount)
      {
         if ( BtDriver_PciCardOpen(btCfg.sourceIdx) )
         {
            cardif.params.cardId = btCfg.cardId;

            if ( cardif.ctl->Open(&cardif, btCfg.wdmStop) )
            {
               if ( cardif.ctl->Configure(btCfg.threadPrio, btCfg.pllType) )
               {
                  if (btCfg.inputSrc != INVALID_INPUT_SOURCE)
                  {  // if source already set, apply it now
                     BtDriver_SetInputSource(btCfg.inputSrc, btCfg.tunerNorm, NULL);
                  }
                  result = TRUE;
               }
               else
               {  // failed to initialize the card (alloc memory for DMA)
                  BtDriver_DsDrvUnload();
               }
            }
         }
         // else: user message already generated by driver
      }
      else
         ifdebug2((btCfg.sourceIdx >= btCardCount), "BtDriver-DsDrvLoad: illegal card index %d >= count %d", btCfg.sourceIdx, btCardCount);

      if ((result == FALSE) && (pciDrvLoaded))
      {
         DsDrvUnload();
         pciDrvLoaded = FALSE;
      }
   }
   else
   {  // failed to load the driver
      MessageBox(NULL, DsDrvGetErrorMsg(loadError), L"nxtvepg driver problem", MB_ICONSTOP | MB_OK | MB_TASKMODAL | MB_SETFOREGROUND);
   }

   return result;
}

// ----------------------------------------------------------------------------
// Check if the parameters are valid for the given source
// - this function is used to warn the user abour parameter mismatch after
//   hardware or driver configuration changes
//
bool BtDriver_CheckCardParams( uint drvType, uint sourceIdx, uint chipId,
                               uint cardType, uint tunerType, uint pll, uint input )
{
   TVCARD tmpCardIf;
   bool   result = FALSE;

   if (drvType == BTDRV_SOURCE_PCI)
   {
      if (btCardCount != CARD_COUNT_UNINITIALIZED)
      {
         if (sourceIdx < btCardCount)
         {
            if (((btCardList[sourceIdx].VendorId << 16) |
                  btCardList[sourceIdx].DeviceId) == chipId)
            {
               if (BtDriver_PciCardGetInterface(&tmpCardIf, (chipId >> 16), CARD_COUNT_UNINITIALIZED) )
               {
                  result = (tmpCardIf.cfg->GetCardName(&tmpCardIf, cardType) != NULL) &&
                           (tmpCardIf.cfg->GetNumInputs(&tmpCardIf) > input);
               }
            }
         }
         else
            debug2("BtDriver-CheckCardParams: source index %d no longer valid (>= %d)", sourceIdx, btCardCount);
      }
      // no PCI scan yet: just do rudimentary checks
      else if (chipId != 0)
      {
         if (BtDriver_PciCardGetInterface(&tmpCardIf, (chipId >> 16), CARD_COUNT_UNINITIALIZED) )
         {
            tmpCardIf.params.cardId = cardType;

            result = (tmpCardIf.cfg->GetCardName(&tmpCardIf, cardType) != NULL) &&
                     (tmpCardIf.cfg->GetNumInputs(&tmpCardIf) > input);
         }
         else
            debug1("BtDriver-CheckCardParams: unknown PCI ID 0x%X", chipId);
      }
      // else: card not configured yet
   }
   return result;
}

// ----------------------------------------------------------------------------
// Set user-configurable hardware parameters
// - called at program start and after config change
// - Important: input source and tuner freq must be set afterwards
//
bool BtDriver_Configure( int sourceIdx, int drvType, int prio, int chipType, int cardType,
                         int tunerType, int pllType, bool wdmStop )
{
   bool sourceChange;
   bool cardTypeChange;
   bool pllChange;
   bool tunerChange;
   bool prioChange;
   int  chipIdx;
   int  oldChipType;
   bool result = TRUE;

   dprintf8("BtDriver-Configure: source=%d drvtype=%d prio=%d chipType=%d cardType=%d tunerType=%d pll=%d wdmStop=%d\n", sourceIdx, drvType, prio, chipType, cardType, tunerType, pllType, wdmStop);
   prio = BtDriver_GetAcqPriority(prio);

   if ( (drvType == BTDRV_SOURCE_PCI) &&
        (btCardCount != CARD_COUNT_UNINITIALIZED) && (sourceIdx < btCardCount) )
   {  // check if the configuration data still matches the hardware
      chipIdx     = btCardList[sourceIdx].chipIdx;
      oldChipType = ((CaptureChips[chipIdx].VendorId << 16) | CaptureChips[chipIdx].DeviceId);
      if (chipType != oldChipType)
      {
         debug3("BtDriver-Configure: PCI chip type of source #%d changed from 0x%X to 0x%X", sourceIdx, oldChipType, chipType);
         cardType = tunerType = pllType = wdmStop = 0;
      }
   }

   // check which values change
   sourceChange   = ((drvType  != btCfg.drvType) ||
                     (sourceIdx != btCfg.sourceIdx) ||
                     (wdmStop  != btCfg.wdmStop));
   cardTypeChange = (cardType  != btCfg.cardId);
   tunerChange    = (tunerType != btCfg.tunerType);
   pllChange      = (pllType   != btCfg.pllType);
   prioChange     = (prio      != btCfg.threadPrio);

   dprintf8("BtDriver-Configure: PREV-MODE: slave=%d pci=%d wdm=%d CHANGES: source:%d cardType:%d tuner:%d pll:%d prio:%d\n", shmSlaveMode, pciDrvLoaded, wdmDrvLoaded, sourceChange, cardTypeChange, tunerChange, pllChange, prioChange);

   // save the new values
   btCfg.drvType    = static_cast<BTDRV_SOURCE_TYPE>(drvType);
   btCfg.sourceIdx  = sourceIdx;
   btCfg.threadPrio = prio;
   btCfg.cardId     = cardType;
   btCfg.tunerType  = tunerType;
   btCfg.pllType    = pllType;
   btCfg.wdmStop    = wdmStop;

   if (shmSlaveMode == FALSE)
   {
      if (pciDrvLoaded || wdmDrvLoaded)
      {  // acquisition already running -> must change parameters on the fly
         cardif.params.cardId = btCfg.cardId;

         if (sourceChange)
         {  // change of TV card (may include switch between DsDrv and WDM)
            BtDriver_StopAcq();

            if (BtDriver_StartAcq() == FALSE)
            {
               if (pVbiBuf != NULL)
                  pVbiBuf->hasFailed = TRUE;
               result = FALSE;
            }
         }
         else
         {  // same source: just update tuner type and PLL
            if (drvType == BTDRV_SOURCE_PCI)
            {
               if (prioChange || pllChange)
               {
                  cardif.ctl->Configure(btCfg.threadPrio, btCfg.pllType);
               }

               if (cardTypeChange)
               {
                  BtDriver_SetInputSource(btCfg.inputSrc, btCfg.tunerNorm, NULL);
               }
            }
            else if (drvType == BTDRV_SOURCE_WDM)
            {  // nothing to do for WDM source
            }
         }
      }
      assert(!(pciDrvLoaded && wdmDrvLoaded));  // only one may be active at the same time
   }
   else
   {  // slave mode -> new card idx
      if (sourceChange)
      {
         BtDriver_StopAcq();

         if (BtDriver_StartAcq() == FALSE)
         {
            if (pVbiBuf != NULL)
               pVbiBuf->hasFailed = TRUE;
            result = FALSE;
         }
      }
   }

   return result;
}
// ---------------------------------------------------------------------------
// Query if acquisition is currently enabled and in which mode
// - return value pointers may be NULL if value not required
//
bool BtDriver_GetState( bool * pEnabled, bool * pHasDriver, uint * pCardIdx )
{
   if (pEnabled != NULL)
      *pEnabled = (shmSlaveMode | pciDrvLoaded | wdmDrvLoaded);
   if (pHasDriver != NULL)
      *pHasDriver = pciDrvLoaded | wdmDrvLoaded;
   if (pCardIdx != NULL)
      *pCardIdx = btCfg.sourceIdx;

   return TRUE;
}

// ---------------------------------------------------------------------------
// Stop and start driver -> toggle slave mode
// - called when a TV application attaches or detaches
// - should not be used to change parameters - used Configure instead
//
bool BtDriver_Restart( void )
{
   bool result;
   uint prevFreq, prevNorm, prevInput;

   // save current input settings into temporary variables
   prevFreq  = btCfg.tunerFreq;
   prevNorm  = btCfg.tunerNorm;
   prevInput = btCfg.inputSrc;

   BtDriver_StopAcq();

   // restore input params
   btCfg.tunerFreq = prevFreq;
   btCfg.tunerNorm = prevNorm;
   btCfg.inputSrc  = prevInput;

   // start acquisition
   result = BtDriver_StartAcq();

   // inform acq control if acq is switched off now
   if (pVbiBuf != NULL)
      pVbiBuf->hasFailed = !result;

   return result;
}

// ---------------------------------------------------------------------------
// Start acquisition
// - the driver is automatically loaded and initialized
//
bool BtDriver_StartAcq( void )
{
   HRESULT wdmResult;
   bool epgHasDriver;
   bool result = FALSE;

   if ( (shmSlaveMode == FALSE) && (pciDrvLoaded == FALSE) && (wdmDrvLoaded == FALSE) )
   {
         if (pVbiBuf != NULL)
         {
            if (btCfg.drvType == BTDRV_SOURCE_PCI)
            {
               // load driver & initialize driver for selected TV card
               if ( BtDriver_DsDrvLoad() )
               {
                  if ( cardif.ctl->StartAcqThread() )
                  {
                     pVbiBuf->hasFailed = FALSE;
                     result = TRUE;
                  }
                  else
                  {
                     BtDriver_DsDrvUnload();
                  }
               }
               assert(pciDrvLoaded == result);

               if ( (btCardCount != CARD_COUNT_UNINITIALIZED) &&
                    (btCfg.sourceIdx >= btCardCount) )
               {
                  WCHAR msgbuf[200];

                  if (btCardCount == 0)
                     wsprintf(msgbuf, L"Cannot start EPG data acquisition because\n"
                                     L"no supported TV capture PCI cards have been found.");
                  else
                     wsprintf(msgbuf, L"Cannot start EPG data acquisition because\n"
                                     L"TV card #%d was not found on the PCI bus\n"
                                     L"(found %d supported TV capture cards)",
                                     btCfg.sourceIdx, btCardCount);
                  MessageBox(NULL, msgbuf, L"nxtvepg driver problem", MB_ICONSTOP | MB_OK | MB_TASKMODAL | MB_SETFOREGROUND);
               }
            }
            else if (btCfg.drvType == BTDRV_SOURCE_NONE)
            {
               // TV card is disabled -> do nothing, return failure result code
            }
            else
               debug1("BtDriver-StartAcq: invalid drv type %d", btCfg.drvType);
         }
   }
   else
   {  // acq already active - should never happen
      debug0("BtDriver-StartAcq: driver already loaded");
      result = TRUE;
   }

   return result;
}

// ---------------------------------------------------------------------------
// Stop acquisition
// - the driver is automatically stopped and removed
// - XXX TODO add recursion detection (in case we're called by exception handler)
//
void BtDriver_StopAcq( void )
{
      if (pciDrvLoaded)
      {
         cardif.ctl->StopAcqThread();
         BtDriver_DsDrvUnload();
      }

   btCfg.inputSrc   = INVALID_INPUT_SOURCE;
}

// ---------------------------------------------------------------------------
// Query error description for last failed operation
// - currently unused because errors are already reported by the driver in WIN32
//
const char * BtDriver_GetLastError( void )
{
   return NULL;
}

// ---------------------------------------------------------------------------
// Initialize the driver module
// - called once at program start
//
bool BtDriver_Init( void )
{
   memset(&vbiBuf, 0, sizeof(vbiBuf));
   pVbiBuf = &vbiBuf;

   memset(&btCfg, 0, sizeof(btCfg));
   memset(&cardif, 0, sizeof(cardif));
   btCfg.drvType   = BTDRV_SOURCE_NONE;
   btCfg.inputSrc  = INVALID_INPUT_SOURCE;
   btCardCount = CARD_COUNT_UNINITIALIZED;
   pciDrvLoaded = FALSE;


   return TRUE;
}

// ---------------------------------------------------------------------------
// Clean up the driver module for exit
// - called once at program termination
//
void BtDriver_Exit( void )
{
   if (pciDrvLoaded || wdmDrvLoaded)
   {  // acq is still running - should never happen
      BtDriver_StopAcq();
   }
   // release dynamically loaded TV card lists
   BtDriver_PciCardsRelease();
}
