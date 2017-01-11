/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <semaphore.h>
#include <errno.h>
#include <malloc.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include "OverrideLog.h"
#include "NfcJniUtil.h"
#include "NfcTag.h"
#include "config.h"
#include "Mutex.h"
#include "IntervalTimer.h"
#include "JavaClassConstants.h"
#include "Pn544Interop.h"
#include <ScopedLocalRef.h>
#include <ScopedPrimitiveArray.h>
#include <string>

extern "C"
{
    #include "nfa_api.h"
    #include "nfa_rw_api.h"
    #include "nfc_brcm_defs.h"
    #include "ndef_utils.h"
    #include "rw_api.h"
    #include "phNxpExtns.h"
}
namespace android
{
    extern nfc_jni_native_data* getNative(JNIEnv *e, jobject o);
    extern bool nfcManager_isNfcActive();
}

extern bool         gActivated;
extern SyncEvent    gDeactivatedEvent;

/*****************************************************************************
**
** public variables and functions
**
*****************************************************************************/
namespace android
{
    bool    gIsTagDeactivating = false;    // flag for nfa callback indicating we are deactivating for RF interface switch
    bool    gIsSelectingRfInterface = false; // flag for nfa callback indicating we are selecting for RF interface switch
}


/*****************************************************************************
**
** private variables and functions
**
*****************************************************************************/
namespace android
{


// Pre-defined tag type values. These must match the values in
// framework Ndef.java for Google public NFC API.
#define NDEF_UNKNOWN_TYPE          (-1)
#define NDEF_TYPE1_TAG             1
#define NDEF_TYPE2_TAG             2
#define NDEF_TYPE3_TAG             3
#define NDEF_TYPE4_TAG             4
#define NDEF_MIFARE_CLASSIC_TAG    101

#define STATUS_CODE_TARGET_LOST    146	// this error code comes from the service

static uint32_t     sCheckNdefCurrentSize = 0;
static tNFA_STATUS  sCheckNdefStatus = 0; //whether tag already contains a NDEF message
static bool         sCheckNdefCapable = false; //whether tag has NDEF capability
static tNFA_HANDLE  sNdefTypeHandlerHandle = NFA_HANDLE_INVALID;
static tNFA_INTF_TYPE   sCurrentRfInterface = NFA_INTERFACE_ISO_DEP;
static std::basic_string<uint8_t> sRxDataBuffer;
static tNFA_STATUS  sRxDataStatus = NFA_STATUS_OK;
static bool         sWaitingForTransceive = false;
static bool         sTransceiveRfTimeout = false;
static Mutex        sRfInterfaceMutex;
static uint32_t     sReadDataLen = 0;
static uint8_t*     sReadData = NULL;
static bool         sIsReadingNdefMessage = false;
static SyncEvent    sReadEvent;
static sem_t        sWriteSem;
static sem_t        sFormatSem;
static SyncEvent    sTransceiveEvent;
static SyncEvent    sReconnectEvent;
static sem_t        sCheckNdefSem;
static SyncEvent    sPresenceCheckEvent;
static sem_t        sMakeReadonlySem;
static IntervalTimer sSwitchBackTimer; // timer used to tell us to switch back to ISO_DEP frame interface
static jboolean     sWriteOk = JNI_FALSE;
static jboolean     sWriteWaitingForComplete = JNI_FALSE;
static bool         sFormatOk = false;
static jboolean     sConnectOk = JNI_FALSE;
static jboolean     sConnectWaitingForComplete = JNI_FALSE;
static bool         sGotDeactivate = false;
static uint32_t     sCheckNdefMaxSize = 0;
static bool         sCheckNdefCardReadOnly = false;
static jboolean     sCheckNdefWaitingForComplete = JNI_FALSE;
static bool         sIsTagPresent = true;
static tNFA_STATUS  sMakeReadonlyStatus = NFA_STATUS_FAILED;
static jboolean     sMakeReadonlyWaitingForComplete = JNI_FALSE;
static int          sCurrentConnectedTargetType = TARGET_TYPE_UNKNOWN;
static int          sCurrentConnectedTargetProtocol = NFC_PROTOCOL_UNKNOWN;
static int reSelect (tNFA_INTF_TYPE rfInterface, bool fSwitchIfNeeded);
static bool switchRfInterface(tNFA_INTF_TYPE rfInterface);


/*******************************************************************************
**
** Function:        nativeNfcTag_abortWaits
**
** Description:     Unblock all thread synchronization objects.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_abortWaits ()
{
    ALOGD ("%s", __func__);
    {
        SyncEventGuard g (sReadEvent);
        sReadEvent.notifyOne ();
    }
    sem_post (&sWriteSem);
    sem_post (&sFormatSem);
    {
        SyncEventGuard g (sTransceiveEvent);
        sTransceiveEvent.notifyOne ();
    }
    {
        SyncEventGuard g (sReconnectEvent);
        sReconnectEvent.notifyOne ();
    }

    sem_post (&sCheckNdefSem);
    {
        SyncEventGuard guard (sPresenceCheckEvent);
        sPresenceCheckEvent.notifyOne ();
    }
    sem_post (&sMakeReadonlySem);
    sCurrentRfInterface = NFA_INTERFACE_ISO_DEP;
    sCurrentConnectedTargetType = TARGET_TYPE_UNKNOWN;
    sCurrentConnectedTargetProtocol = NFC_PROTOCOL_UNKNOWN;
}

/*******************************************************************************
**
** Function:        nativeNfcTag_doReadCompleted
**
** Description:     Receive the completion status of read operation.  Called by
**                  NFA_READ_CPLT_EVT.
**                  status: Status of operation.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_doReadCompleted (tNFA_STATUS status)
{
    ALOGD ("%s: status=0x%X; is reading=%u", __func__, status, sIsReadingNdefMessage);

    if (sIsReadingNdefMessage == false)
        return; //not reading NDEF message right now, so just return

    if (status != NFA_STATUS_OK)
    {
        sReadDataLen = 0;
        if (sReadData)
            free (sReadData);
        sReadData = NULL;
    }
    SyncEventGuard g (sReadEvent);
    sReadEvent.notifyOne ();
}


/*******************************************************************************
**
** Function:        nativeNfcTag_setRfInterface
**
** Description:     Set rf interface.
**
** Returns:         void
**
*******************************************************************************/
void nativeNfcTag_setRfInterface (tNFA_INTF_TYPE rfInterface)
{
    sCurrentRfInterface = rfInterface;
}


/*******************************************************************************
**
** Function:        ndefHandlerCallback
**
** Description:     Receive NDEF-message related events from stack.
**                  event: Event code.
**                  p_data: Event data.
**
** Returns:         None
**
*******************************************************************************/
static void ndefHandlerCallback (tNFA_NDEF_EVT event, tNFA_NDEF_EVT_DATA *eventData)
{
    ALOGD ("%s: event=%u, eventData=%p", __func__, event, eventData);

    switch (event)
    {
    case NFA_NDEF_REGISTER_EVT:
        {
            tNFA_NDEF_REGISTER& ndef_reg = eventData->ndef_reg;
            ALOGD ("%s: NFA_NDEF_REGISTER_EVT; status=0x%X; h=0x%X", __func__, ndef_reg.status, ndef_reg.ndef_type_handle);
            sNdefTypeHandlerHandle = ndef_reg.ndef_type_handle;
        }
        break;

    case NFA_NDEF_DATA_EVT:
        {
            ALOGD ("%s: NFA_NDEF_DATA_EVT; data_len = %lu", __func__, eventData->ndef_data.len);
            sReadDataLen = eventData->ndef_data.len;
            sReadData = (uint8_t*) malloc (sReadDataLen);
            memcpy (sReadData, eventData->ndef_data.p_data, eventData->ndef_data.len);
        }
        break;

    default:
        ALOGE ("%s: Unknown event %u ????", __func__, event);
        break;
    }
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doRead
**
** Description:     Read the NDEF message on the tag.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         NDEF message.
**
*******************************************************************************/
static jbyteArray nativeNfcTag_doRead (JNIEnv* e, jobject)
{
    ALOGD ("%s: enter", __func__);
    tNFA_STATUS status = NFA_STATUS_FAILED;
    jbyteArray buf = NULL;

    sReadDataLen = 0;
    if (sReadData != NULL)
    {
        free (sReadData);
        sReadData = NULL;
    }

    if (sCheckNdefCurrentSize > 0)
    {
        {
            SyncEventGuard g (sReadEvent);
            sIsReadingNdefMessage = true;
            if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
            {
                status = EXTNS_MfcReadNDef ();
            }
            else
            {
                status = NFA_RwReadNDef ();
            }
            sReadEvent.wait (); //wait for NFA_READ_CPLT_EVT
        }
        sIsReadingNdefMessage = false;

        if (sReadDataLen > 0) //if stack actually read data from the tag
        {
            ALOGD ("%s: read %u bytes", __func__, sReadDataLen);
            buf = e->NewByteArray (sReadDataLen);
            e->SetByteArrayRegion (buf, 0, sReadDataLen, (jbyte*) sReadData);
        }
    }
    else
    {
        ALOGD ("%s: create empty buffer", __func__);
        sReadDataLen = 0;
        sReadData = (uint8_t*) malloc (1);
        buf = e->NewByteArray (sReadDataLen);
        e->SetByteArrayRegion (buf, 0, sReadDataLen, (jbyte*) sReadData);
    }

    if (sReadData)
    {
        free (sReadData);
        sReadData = NULL;
    }
    sReadDataLen = 0;

    ALOGD ("%s: exit", __func__);
    return buf;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doWriteStatus
**
** Description:     Receive the completion status of write operation.  Called
**                  by NFA_WRITE_CPLT_EVT.
**                  isWriteOk: Status of operation.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_doWriteStatus (jboolean isWriteOk)
{
    if (sWriteWaitingForComplete != JNI_FALSE)
    {
        sWriteWaitingForComplete = JNI_FALSE;
        sWriteOk = isWriteOk;
        sem_post (&sWriteSem);
    }
}


/*******************************************************************************
**
** Function:        nativeNfcTag_formatStatus
**
** Description:     Receive the completion status of format operation.  Called
**                  by NFA_FORMAT_CPLT_EVT.
**                  isOk: Status of operation.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_formatStatus (bool isOk)
{
    sFormatOk = isOk;
    sem_post (&sFormatSem);
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doWrite
**
** Description:     Write a NDEF message to the tag.
**                  e: JVM environment.
**                  o: Java object.
**                  buf: Contains a NDEF message.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean nativeNfcTag_doWrite (JNIEnv* e, jobject, jbyteArray buf)
{
    jboolean result = JNI_FALSE;
    tNFA_STATUS status = 0;
    const int maxBufferSize = 1024;
    uint8_t buffer[maxBufferSize] = { 0 };
    uint32_t curDataSize = 0;

    ScopedByteArrayRO bytes(e, buf);
    uint8_t* p_data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&bytes[0])); // TODO: const-ness API bug in NFA_RwWriteNDef!

    ALOGD ("%s: enter; len = %zu", __func__, bytes.size());

    /* Create the write semaphore */
    if (sem_init (&sWriteSem, 0, 0) == -1)
    {
        ALOGE ("%s: semaphore creation failed (errno=0x%08x)", __func__, errno);
        return JNI_FALSE;
    }

    sWriteWaitingForComplete = JNI_TRUE;
    if (sCheckNdefStatus == NFA_STATUS_FAILED)
    {
        //if tag does not contain a NDEF message
        //and tag is capable of storing NDEF message
        if (sCheckNdefCapable)
        {
            ALOGD ("%s: try format", __func__);
            sem_init (&sFormatSem, 0, 0);
            sFormatOk = false;
            if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
            {
                static uint8_t mfc_key1[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                static uint8_t mfc_key2[6] = {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7};

                status = EXTNS_MfcFormatTag (mfc_key1, sizeof(mfc_key1));
                if (status != NFA_STATUS_OK)
                {
                    ALOGE ("%s: can't format mifare classic tag", __func__);
                    sem_destroy (&sFormatSem);
                    goto TheEnd;
                }

                if (sFormatOk == false) //if format operation failed
                {
                    sem_wait (&sFormatSem);
                    sem_destroy (&sFormatSem);
                    sem_init (&sFormatSem, 0, 0);
                    status = EXTNS_MfcFormatTag (mfc_key2, sizeof(mfc_key2));
                    if (status != NFA_STATUS_OK)
                    {
                        ALOGE ("%s: can't format mifare classic tag", __func__);
                        sem_destroy (&sFormatSem);
                        goto TheEnd;
                    }
                }
            }
            else
            {
                status = NFA_RwFormatTag ();
            }
            sem_wait (&sFormatSem);
            sem_destroy (&sFormatSem);
            if (sFormatOk == false) //if format operation failed
                goto TheEnd;
        }
        ALOGD ("%s: try write", __func__);
        status = NFA_RwWriteNDef (p_data, bytes.size());
    }
    else if (bytes.size() == 0)
    {
        //if (NXP TagWriter wants to erase tag) then create and write an empty ndef message
        NDEF_MsgInit (buffer, maxBufferSize, &curDataSize);
        status = NDEF_MsgAddRec (buffer, maxBufferSize, &curDataSize, NDEF_TNF_EMPTY, NULL, 0, NULL, 0, NULL, 0);
        ALOGD ("%s: create empty ndef msg; status=%u; size=%lu", __func__, status, curDataSize);
        if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
        {
            status = EXTNS_MfcWriteNDef (buffer, curDataSize);
        }
        else
        {
            status = NFA_RwWriteNDef (buffer, curDataSize);
        }
    }
    else
    {
        ALOGD ("%s: NFA_RwWriteNDef", __func__);
        if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
        {
            status = EXTNS_MfcWriteNDef (p_data, bytes.size());
        }
        else
        {
            status = NFA_RwWriteNDef (p_data, bytes.size());
        }
    }

    if (status != NFA_STATUS_OK)
    {
        ALOGE ("%s: write/format error=%d", __func__, status);
        goto TheEnd;
    }

    /* Wait for write completion status */
    sWriteOk = false;
    if (sem_wait (&sWriteSem))
    {
        ALOGE ("%s: wait semaphore (errno=0x%08x)", __func__, errno);
        goto TheEnd;
    }

    result = sWriteOk;

TheEnd:
    /* Destroy semaphore */
    if (sem_destroy (&sWriteSem))
    {
        ALOGE ("%s: failed destroy semaphore (errno=0x%08x)", __func__, errno);
    }
    sWriteWaitingForComplete = JNI_FALSE;
    ALOGD ("%s: exit; result=%d", __func__, result);
    return result;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doConnectStatus
**
** Description:     Receive the completion status of connect operation.
**                  isConnectOk: Status of the operation.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_doConnectStatus (jboolean isConnectOk)
{
    if (EXTNS_GetConnectFlag () == TRUE)
    {
        EXTNS_MfcActivated ();
        EXTNS_SetConnectFlag (FALSE);
        return;
    }

    if (sConnectWaitingForComplete != JNI_FALSE)
    {
        sConnectWaitingForComplete = JNI_FALSE;
        sConnectOk = isConnectOk;
        SyncEventGuard g (sReconnectEvent);
        sReconnectEvent.notifyOne ();
    }
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doDeactivateStatus
**
** Description:     Receive the completion status of deactivate operation.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_doDeactivateStatus (int status)
{
    if (EXTNS_GetDeactivateFlag () == TRUE)
    {
        EXTNS_MfcDisconnect ();
        EXTNS_SetDeactivateFlag (FALSE);
        return;
    }

    sGotDeactivate = (status == 0);

    SyncEventGuard g (sReconnectEvent);
    sReconnectEvent.notifyOne ();
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doConnect
**
** Description:     Connect to the tag in RF field.
**                  e: JVM environment.
**                  o: Java object.
**                  targetHandle: Handle of the tag.
**
** Returns:         Must return NXP status code, which NFC service expects.
**
*******************************************************************************/
static jint nativeNfcTag_doConnect (JNIEnv*, jobject, jint targetHandle)
{
    ALOGD ("%s: targetHandle = %d", __func__, targetHandle);
    int i = targetHandle;
    NfcTag& natTag = NfcTag::getInstance ();
    int retCode = NFCSTATUS_SUCCESS;

    if (i >= NfcTag::MAX_NUM_TECHNOLOGY)
    {
        ALOGE ("%s: Handle not found", __func__);
        retCode = NFCSTATUS_FAILED;
        goto TheEnd;
    }

    if (natTag.getActivationState() != NfcTag::Active)
    {
        ALOGE ("%s: tag already deactivated", __func__);
        retCode = NFCSTATUS_FAILED;
        goto TheEnd;
    }

    sCurrentConnectedTargetType = natTag.mTechList[i];
    sCurrentConnectedTargetProtocol = natTag.mTechLibNfcTypes[i];

    if (sCurrentConnectedTargetProtocol != NFC_PROTOCOL_ISO_DEP)
    {
        ALOGD ("%s() Nfc type = %d, do nothing for non ISO_DEP", __func__, sCurrentConnectedTargetProtocol);
        retCode = NFCSTATUS_SUCCESS;
        goto TheEnd;
    }

    if (sCurrentConnectedTargetType == TARGET_TYPE_ISO14443_3A || sCurrentConnectedTargetType == TARGET_TYPE_ISO14443_3B)
    {
        ALOGD ("%s: switching to tech: %d need to switch rf intf to frame", __func__, sCurrentConnectedTargetType);
        retCode = switchRfInterface(NFA_INTERFACE_FRAME) ? NFA_STATUS_OK : NFA_STATUS_FAILED;
    }
    else
    {
        retCode = switchRfInterface(NFA_INTERFACE_ISO_DEP) ? NFA_STATUS_OK : NFA_STATUS_FAILED;
    }

TheEnd:
    ALOGD ("%s: exit 0x%X", __func__, retCode);
    return retCode;
}


/*******************************************************************************
**
** Function:        reSelect
**
** Description:     Deactivates the tag and re-selects it with the specified
**                  rf interface.
**
** Returns:         status code, 0 on success, 1 on failure,
**                  146 (defined in service) on tag lost
**
*******************************************************************************/
static int reSelect (tNFA_INTF_TYPE rfInterface, bool fSwitchIfNeeded)
{
    ALOGD ("%s: enter; rf intf = %d, current intf = %d", __func__, rfInterface, sCurrentRfInterface);

    sRfInterfaceMutex.lock ();

    if (fSwitchIfNeeded && (rfInterface == sCurrentRfInterface))
    {
        // already in the requested interface
        sRfInterfaceMutex.unlock ();
        return 0;   // success
    }

    NfcTag& natTag = NfcTag::getInstance ();

    tNFA_STATUS status;
    int rVal = 1;

    do
    {
        //if tag has shutdown, abort this method
        if (NfcTag::getInstance ().isNdefDetectionTimedOut())
        {
            ALOGD ("%s: ndef detection timeout; break", __func__);
            rVal = STATUS_CODE_TARGET_LOST;
            break;
        }

        {
            SyncEventGuard g (sReconnectEvent);
            gIsTagDeactivating = true;
            sGotDeactivate = false;
            ALOGD ("%s: deactivate to sleep", __func__);
            if (NFA_STATUS_OK != (status = NFA_Deactivate (TRUE))) //deactivate to sleep state
            {
                ALOGE ("%s: deactivate failed, status = %d", __func__, status);
                break;
            }

            if (sReconnectEvent.wait (1000) == false) //if timeout occurred
            {
                ALOGE ("%s: timeout waiting for deactivate", __func__);
            }
        }

        if (!sGotDeactivate)
        {
            rVal = STATUS_CODE_TARGET_LOST;
            break;
        }

        if (NfcTag::getInstance ().getActivationState () != NfcTag::Sleep)
        {
            ALOGE ("%s: tag is not in sleep", __func__);
            rVal = STATUS_CODE_TARGET_LOST;
            break;
        }

        gIsTagDeactivating = false;

        {
            SyncEventGuard g2 (sReconnectEvent);

            sConnectWaitingForComplete = JNI_TRUE;
            ALOGD ("%s: select interface %u", __func__, rfInterface);
            gIsSelectingRfInterface = true;
            if (NFA_STATUS_OK != (status = NFA_Select (natTag.mTechHandles[0], natTag.mTechLibNfcTypes[0], rfInterface)))
            {
                ALOGE ("%s: NFA_Select failed, status = %d", __func__, status);
                break;
            }

            sConnectOk = false;
            if (sReconnectEvent.wait (1000) == false) //if timeout occured
            {
                ALOGE ("%s: timeout waiting for select", __func__);
                break;
            }
        }

        ALOGD("%s: select completed; sConnectOk=%d", __func__, sConnectOk);
        if (NfcTag::getInstance ().getActivationState () != NfcTag::Active)
        {
            ALOGE("%s: tag is not active", __func__);
            rVal = STATUS_CODE_TARGET_LOST;
            break;
        }
        if (sConnectOk)
        {
            rVal = 0;   // success
            sCurrentRfInterface = rfInterface;
        }
        else
        {
            rVal = 1;
        }
    } while (0);

    sConnectWaitingForComplete = JNI_FALSE;
    gIsTagDeactivating = false;
    gIsSelectingRfInterface = false;
    sRfInterfaceMutex.unlock ();
    ALOGD ("%s: exit; status=%d", __func__, rVal);
    return rVal;
}

/*******************************************************************************
**
** Function:        switchRfInterface
**
** Description:     Switch controller's RF interface to frame, ISO-DEP, or NFC-DEP.
**                  rfInterface: Type of RF interface.
**
** Returns:         True if ok.
**
*******************************************************************************/
static bool switchRfInterface (tNFA_INTF_TYPE rfInterface)
{
    NfcTag& natTag = NfcTag::getInstance ();

    if (sCurrentConnectedTargetProtocol != NFC_PROTOCOL_ISO_DEP)
    {
        ALOGD ("%s: protocol: %d not ISO_DEP, do nothing", __func__, natTag.mTechLibNfcTypes[0]);
        return true;
    }

    ALOGD ("%s: new rf intf = %d, cur rf intf = %d", __func__, rfInterface, sCurrentRfInterface);

    return (0 == reSelect(rfInterface, true));
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doReconnect
**
** Description:     Re-connect to the tag in RF field.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Status code.
**
*******************************************************************************/
static jint nativeNfcTag_doReconnect (JNIEnv*, jobject)
{
    ALOGD ("%s: enter", __func__);
    int retCode = NFCSTATUS_SUCCESS;
    NfcTag& natTag = NfcTag::getInstance ();

    if (natTag.getActivationState() != NfcTag::Active)
    {
        ALOGE ("%s: tag already deactivated", __func__);
        retCode = NFCSTATUS_FAILED;
        goto TheEnd;
    }

    // special case for Kovio
    if (sCurrentConnectedTargetProtocol == TARGET_TYPE_KOVIO_BARCODE)
    {
        ALOGD ("%s: fake out reconnect for Kovio", __func__);
        goto TheEnd;
    }

     // this is only supported for type 2 or 4 (ISO_DEP) tags
    if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_ISO_DEP)
        retCode = reSelect (NFA_INTERFACE_ISO_DEP, false);
    else if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_T2T)
        retCode = reSelect (NFA_INTERFACE_FRAME, false);
    else if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
        retCode = reSelect (NFA_INTERFACE_MIFARE, false);

TheEnd:
    ALOGD ("%s: exit 0x%X", __func__, retCode);
    return retCode;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doHandleReconnect
**
** Description:     Re-connect to the tag in RF field.
**                  e: JVM environment.
**                  o: Java object.
**                  targetHandle: Handle of the tag.
**
** Returns:         Status code.
**
*******************************************************************************/
static jint nativeNfcTag_doHandleReconnect (JNIEnv *e, jobject o, jint targetHandle)
{
    ALOGD ("%s: targetHandle = %d", __func__, targetHandle);
    return nativeNfcTag_doConnect (e, o, targetHandle);
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doDisconnect
**
** Description:     Deactivate the RF field.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean nativeNfcTag_doDisconnect (JNIEnv*, jobject)
{
    ALOGD ("%s: enter", __func__);
    tNFA_STATUS nfaStat = NFA_STATUS_OK;

    NfcTag::getInstance().resetAllTransceiveTimeouts ();

    if (NfcTag::getInstance ().getActivationState () != NfcTag::Active)
    {
        ALOGE ("%s: tag already deactivated", __func__);
        goto TheEnd;
    }

    nfaStat = NFA_Deactivate (FALSE);
    if (nfaStat != NFA_STATUS_OK)
        ALOGE ("%s: deactivate failed; error=0x%X", __func__, nfaStat);

TheEnd:
    ALOGD ("%s: exit", __func__);
    return (nfaStat == NFA_STATUS_OK) ? JNI_TRUE : JNI_FALSE;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doTransceiveStatus
**
** Description:     Receive the completion status of transceive operation.
**                  status: operation status.
**                  buf: Contains tag's response.
**                  bufLen: Length of buffer.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_doTransceiveStatus (tNFA_STATUS status, uint8_t* buf, uint32_t bufLen)
{
    SyncEventGuard g (sTransceiveEvent);
    ALOGD ("%s: data len=%d", __func__, bufLen);

    if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
    {
       if (EXTNS_GetCallBackFlag () == FALSE)
       {
           EXTNS_MfcCallBack (buf, bufLen);
           return;
       }
    }

    if (!sWaitingForTransceive)
    {
        ALOGE ("%s: drop data", __func__);
        return;
    }
    sRxDataStatus = status;
    if (sRxDataStatus == NFA_STATUS_OK || sRxDataStatus == NFA_STATUS_CONTINUE)
        sRxDataBuffer.append (buf, bufLen);

    if (sRxDataStatus == NFA_STATUS_OK)
        sTransceiveEvent.notifyOne ();
}


void nativeNfcTag_notifyRfTimeout ()
{
    SyncEventGuard g (sTransceiveEvent);
    ALOGD ("%s: waiting for transceive: %d", __func__, sWaitingForTransceive);
    if (!sWaitingForTransceive)
        return;

    sTransceiveRfTimeout = true;

    sTransceiveEvent.notifyOne ();
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doTransceive
**
** Description:     Send raw data to the tag; receive tag's response.
**                  e: JVM environment.
**                  o: Java object.
**                  raw: Not used.
**                  statusTargetLost: Whether tag responds or times out.
**
** Returns:         Response from tag.
**
*******************************************************************************/
static jbyteArray nativeNfcTag_doTransceive (JNIEnv* e, jobject o, jbyteArray data, jboolean raw, jintArray statusTargetLost)
{
    int timeout = NfcTag::getInstance ().getTransceiveTimeout (sCurrentConnectedTargetType);
    ALOGD ("%s: enter; raw=%u; timeout = %d", __func__, raw, timeout);

    bool waitOk = false;
    bool isNack = false;
    jint *targetLost = NULL;
    tNFA_STATUS status;

    if (NfcTag::getInstance ().getActivationState () != NfcTag::Active)
    {
        if (statusTargetLost)
        {
            targetLost = e->GetIntArrayElements (statusTargetLost, 0);
            if (targetLost)
                *targetLost = 1; //causes NFC service to throw TagLostException
            e->ReleaseIntArrayElements (statusTargetLost, targetLost, 0);
        }
        ALOGD ("%s: tag not active", __func__);
        return NULL;
    }

    NfcTag& natTag = NfcTag::getInstance ();

    // get input buffer and length from java call
    ScopedByteArrayRO bytes(e, data);
    uint8_t* buf = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&bytes[0])); // TODO: API bug; NFA_SendRawFrame should take const*!
    size_t bufLen = bytes.size();

    if (statusTargetLost)
    {
        targetLost = e->GetIntArrayElements (statusTargetLost, 0);
        if (targetLost)
            *targetLost = 0; //success, tag is still present
    }

    sSwitchBackTimer.kill ();
    ScopedLocalRef<jbyteArray> result(e, NULL);
    do
    {
        {
            SyncEventGuard g (sTransceiveEvent);
            sTransceiveRfTimeout = false;
            sWaitingForTransceive = true;
            sRxDataStatus = NFA_STATUS_OK;
            sRxDataBuffer.clear ();

            if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
            {
                status = EXTNS_MfcTransceive (buf, bufLen);
            }
            else
            {
                status = NFA_SendRawFrame (buf, bufLen,
                        NFA_DM_DEFAULT_PRESENCE_CHECK_START_DELAY);
            }

            if (status != NFA_STATUS_OK)
            {
                ALOGE ("%s: fail send; error=%d", __func__, status);
                break;
            }
            waitOk = sTransceiveEvent.wait (timeout);
        }

        if (waitOk == false || sTransceiveRfTimeout) //if timeout occurred
        {
            ALOGE ("%s: wait response timeout", __func__);
            if (targetLost)
                *targetLost = 1; //causes NFC service to throw TagLostException
            break;
        }

        if (NfcTag::getInstance ().getActivationState () != NfcTag::Active)
        {
            ALOGE ("%s: already deactivated", __func__);
            if (targetLost)
                *targetLost = 1; //causes NFC service to throw TagLostException
            break;
        }

        ALOGD ("%s: response %zu bytes", __func__, sRxDataBuffer.size());

        if ((natTag.getProtocol () == NFA_PROTOCOL_T2T) &&
            natTag.isT2tNackResponse (sRxDataBuffer.data(), sRxDataBuffer.size()))
        {
            isNack = true;
        }

        if (sRxDataBuffer.size() > 0)
        {
            if (isNack)
            {
                //Some Mifare Ultralight C tags enter the HALT state after it
                //responds with a NACK.  Need to perform a "reconnect" operation
                //to wake it.
                ALOGD ("%s: try reconnect", __func__);
                nativeNfcTag_doReconnect (NULL, NULL);
                ALOGD ("%s: reconnect finish", __func__);
            }
            else if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
            {
                uint32_t transDataLen = sRxDataBuffer.size ();
                uint8_t *transData = (uint8_t *) sRxDataBuffer.data ();
                if (EXTNS_CheckMfcResponse (&transData, &transDataLen) == NFCSTATUS_FAILED)
                {
                    nativeNfcTag_doReconnect (e, o);
                }
                else
                {
                    if (transDataLen != 0)
                    {
                        result.reset (e->NewByteArray (transDataLen));
                    }
                    if (result.get () != NULL)
                    {
                        e->SetByteArrayRegion (result.get (), 0, transDataLen, (const jbyte *) transData);
                    }
                    else
                        ALOGE ("%s: Failed to allocate java byte array", __func__);
                }
            }
            else
            {
                // marshall data to java for return
                result.reset(e->NewByteArray(sRxDataBuffer.size()));
                if (result.get() != NULL)
                {
                    e->SetByteArrayRegion(result.get(), 0, sRxDataBuffer.size(), (const jbyte *) sRxDataBuffer.data());
                }
                else
                    ALOGE ("%s: Failed to allocate java byte array", __func__);
            } // else a nack is treated as a transceive failure to the upper layers

            sRxDataBuffer.clear();
        }
    } while (0);

    sWaitingForTransceive = false;
    if (targetLost)
        e->ReleaseIntArrayElements (statusTargetLost, targetLost, 0);

    ALOGD ("%s: exit", __func__);
    return result.release();
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doGetNdefType
**
** Description:     Retrieve the type of tag.
**                  e: JVM environment.
**                  o: Java object.
**                  libnfcType: Type of tag represented by JNI.
**                  javaType: Not used.
**
** Returns:         Type of tag represented by NFC Service.
**
*******************************************************************************/
static jint nativeNfcTag_doGetNdefType (JNIEnv*, jobject, jint libnfcType, jint javaType)
{
    ALOGD ("%s: enter; libnfc type=%d; java type=%d", __func__, libnfcType, javaType);
    jint ndefType = NDEF_UNKNOWN_TYPE;

    // For NFA, libnfcType is mapped to the protocol value received
    // in the NFA_ACTIVATED_EVT and NFA_DISC_RESULT_EVT event.
    if (NFA_PROTOCOL_T1T == libnfcType)
    {
        ndefType = NDEF_TYPE1_TAG;
    }
    else if (NFA_PROTOCOL_T2T == libnfcType)
    {
        ndefType = NDEF_TYPE2_TAG;
    }
    else if (NFA_PROTOCOL_T3T == libnfcType)
    {
        ndefType = NDEF_TYPE3_TAG;
    }
    else if (NFA_PROTOCOL_ISO_DEP == libnfcType)
    {
        ndefType = NDEF_TYPE4_TAG;
    }
    else if (NFA_PROTOCOL_MIFARE == libnfcType)
    {
        ndefType = NDEF_MIFARE_CLASSIC_TAG;
    }
    else
    {
        /* NFA_PROTOCOL_ISO15693, NFA_PROTOCOL_INVALID and others */
        ndefType = NDEF_UNKNOWN_TYPE;
    }
    ALOGD ("%s: exit; ndef type=%d", __func__, ndefType);
    return ndefType;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doCheckNdefResult
**
** Description:     Receive the result of checking whether the tag contains a NDEF
**                  message.  Called by the NFA_NDEF_DETECT_EVT.
**                  status: Status of the operation.
**                  maxSize: Maximum size of NDEF message.
**                  currentSize: Current size of NDEF message.
**                  flags: Indicate various states.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_doCheckNdefResult (tNFA_STATUS status, uint32_t maxSize, uint32_t currentSize, uint8_t flags)
{
    //this function's flags parameter is defined using the following macros
    //in nfc/include/rw_api.h;
    //#define RW_NDEF_FL_READ_ONLY  0x01    /* Tag is read only              */
    //#define RW_NDEF_FL_FORMATED   0x02    /* Tag formated for NDEF         */
    //#define RW_NDEF_FL_SUPPORTED  0x04    /* NDEF supported by the tag     */
    //#define RW_NDEF_FL_UNKNOWN    0x08    /* Unable to find if tag is ndef capable/formated/read only */
    //#define RW_NDEF_FL_FORMATABLE 0x10    /* Tag supports format operation */

    if (!sCheckNdefWaitingForComplete)
    {
        ALOGE ("%s: not waiting", __func__);
        return;
    }

    if (flags & RW_NDEF_FL_READ_ONLY)
        ALOGD ("%s: flag read-only", __func__);
    if (flags & RW_NDEF_FL_FORMATED)
        ALOGD ("%s: flag formatted for ndef", __func__);
    if (flags & RW_NDEF_FL_SUPPORTED)
        ALOGD ("%s: flag ndef supported", __func__);
    if (flags & RW_NDEF_FL_UNKNOWN)
        ALOGD ("%s: flag all unknown", __func__);
    if (flags & RW_NDEF_FL_FORMATABLE)
        ALOGD ("%s: flag formattable", __func__);

    sCheckNdefWaitingForComplete = JNI_FALSE;
    sCheckNdefStatus = status;
    if (sCheckNdefStatus != NFA_STATUS_OK && sCheckNdefStatus != NFA_STATUS_TIMEOUT)
        sCheckNdefStatus = NFA_STATUS_FAILED;
    sCheckNdefCapable = false; //assume tag is NOT ndef capable
    if (sCheckNdefStatus == NFA_STATUS_OK)
    {
        //NDEF content is on the tag
        sCheckNdefMaxSize = maxSize;
        sCheckNdefCurrentSize = currentSize;
        sCheckNdefCardReadOnly = flags & RW_NDEF_FL_READ_ONLY;
        sCheckNdefCapable = true;
    }
    else if (sCheckNdefStatus == NFA_STATUS_FAILED)
    {
        //no NDEF content on the tag
        sCheckNdefMaxSize = 0;
        sCheckNdefCurrentSize = 0;
        sCheckNdefCardReadOnly = flags & RW_NDEF_FL_READ_ONLY;
        if ((flags & RW_NDEF_FL_UNKNOWN) == 0) //if stack understands the tag
        {
            if (flags & RW_NDEF_FL_SUPPORTED) //if tag is ndef capable
                sCheckNdefCapable = true;
        }
    }
    else
    {
        ALOGE ("%s: unknown status=0x%X", __func__, status);
        sCheckNdefMaxSize = 0;
        sCheckNdefCurrentSize = 0;
        sCheckNdefCardReadOnly = false;
    }
    sem_post (&sCheckNdefSem);
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doCheckNdef
**
** Description:     Does the tag contain a NDEF message?
**                  e: JVM environment.
**                  o: Java object.
**                  ndefInfo: NDEF info.
**
** Returns:         Status code; 0 is success.
**
*******************************************************************************/
static jint nativeNfcTag_doCheckNdef (JNIEnv* e, jobject o, jintArray ndefInfo)
{
    tNFA_STATUS status = NFA_STATUS_FAILED;
    jint* ndef = NULL;

    ALOGD ("%s: enter", __func__);

    // special case for Kovio
    if (sCurrentConnectedTargetProtocol == TARGET_TYPE_KOVIO_BARCODE)
    {
        ALOGD ("%s: Kovio tag, no NDEF", __func__);
        ndef = e->GetIntArrayElements (ndefInfo, 0);
        ndef[0] = 0;
        ndef[1] = NDEF_MODE_READ_ONLY;
        e->ReleaseIntArrayElements (ndefInfo, ndef, 0);
        return NFA_STATUS_FAILED;
    }
    else if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
    {
        nativeNfcTag_doReconnect (e, o);
    }

    /* Create the write semaphore */
    if (sem_init (&sCheckNdefSem, 0, 0) == -1)
    {
        ALOGE ("%s: Check NDEF semaphore creation failed (errno=0x%08x)", __func__, errno);
        return JNI_FALSE;
    }

    if (NfcTag::getInstance ().getActivationState () != NfcTag::Active)
    {
        ALOGE ("%s: tag already deactivated", __func__);
        goto TheEnd;
    }

    ALOGD ("%s: try NFA_RwDetectNDef", __func__);
    sCheckNdefWaitingForComplete = JNI_TRUE;

    if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
    {
        status = EXTNS_MfcCheckNDef ();
    }
    else
    {
        status = NFA_RwDetectNDef ();
    }

    if (status != NFA_STATUS_OK)
    {
        ALOGE ("%s: NFA_RwDetectNDef failed, status = 0x%X", __func__, status);
        goto TheEnd;
    }

    /* Wait for check NDEF completion status */
    if (sem_wait (&sCheckNdefSem))
    {
        ALOGE ("%s: Failed to wait for check NDEF semaphore (errno=0x%08x)", __func__, errno);
        goto TheEnd;
    }

    if (sCheckNdefStatus == NFA_STATUS_OK)
    {
        //stack found a NDEF message on the tag
        ndef = e->GetIntArrayElements (ndefInfo, 0);
        if (NfcTag::getInstance ().getProtocol () == NFA_PROTOCOL_T1T)
            ndef[0] = NfcTag::getInstance ().getT1tMaxMessageSize ();
        else
            ndef[0] = sCheckNdefMaxSize;
        if (sCheckNdefCardReadOnly)
            ndef[1] = NDEF_MODE_READ_ONLY;
        else
            ndef[1] = NDEF_MODE_READ_WRITE;
        e->ReleaseIntArrayElements (ndefInfo, ndef, 0);
        status = NFA_STATUS_OK;
    }
    else if (sCheckNdefStatus == NFA_STATUS_FAILED)
    {
        //stack did not find a NDEF message on the tag;
        ndef = e->GetIntArrayElements (ndefInfo, 0);
        if (NfcTag::getInstance ().getProtocol () == NFA_PROTOCOL_T1T)
            ndef[0] = NfcTag::getInstance ().getT1tMaxMessageSize ();
        else
            ndef[0] = sCheckNdefMaxSize;
        if (sCheckNdefCardReadOnly)
            ndef[1] = NDEF_MODE_READ_ONLY;
        else
            ndef[1] = NDEF_MODE_READ_WRITE;
        e->ReleaseIntArrayElements (ndefInfo, ndef, 0);
        status = NFA_STATUS_FAILED;
    }
    else if ((sCheckNdefStatus == NFA_STATUS_TIMEOUT) && (NfcTag::getInstance ().getProtocol() == NFC_PROTOCOL_ISO_DEP))
    {
        pn544InteropStopPolling ();
        status = sCheckNdefStatus;
    }
    else
    {
        ALOGD ("%s: unknown status 0x%X", __func__, sCheckNdefStatus);
        status = sCheckNdefStatus;
    }

    /* Reconnect Mifare Classic Tag for furture use */
    if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
    {
        nativeNfcTag_doReconnect (e, o);
    }

TheEnd:
    /* Destroy semaphore */
    if (sem_destroy (&sCheckNdefSem))
    {
        ALOGE ("%s: Failed to destroy check NDEF semaphore (errno=0x%08x)", __func__, errno);
    }
    sCheckNdefWaitingForComplete = JNI_FALSE;
    ALOGD ("%s: exit; status=0x%X", __func__, status);
    return status;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_resetPresenceCheck
**
** Description:     Reset variables related to presence-check.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_resetPresenceCheck ()
{
    sIsTagPresent = true;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doPresenceCheckResult
**
** Description:     Receive the result of presence-check.
**                  status: Result of presence-check.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_doPresenceCheckResult (tNFA_STATUS status)
{
    SyncEventGuard guard (sPresenceCheckEvent);
    sIsTagPresent = status == NFA_STATUS_OK;
    sPresenceCheckEvent.notifyOne ();
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doPresenceCheck
**
** Description:     Check if the tag is in the RF field.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if tag is in RF field.
**
*******************************************************************************/
static jboolean nativeNfcTag_doPresenceCheck (JNIEnv*, jobject)
{
    ALOGD ("%s", __func__);
    tNFA_STATUS status = NFA_STATUS_OK;
    jboolean isPresent = JNI_FALSE;

    // Special case for Kovio.  The deactivation would have already occurred
    // but was ignored so that normal tag opertions could complete.  Now we
    // want to process as if the deactivate just happened.
    if (sCurrentConnectedTargetProtocol == TARGET_TYPE_KOVIO_BARCODE)
    {
        ALOGD ("%s: Kovio, force deactivate handling", __func__);
        tNFA_DEACTIVATED deactivated = {NFA_DEACTIVATE_TYPE_IDLE};
        {
            SyncEventGuard g (gDeactivatedEvent);
            gActivated = false; //guard this variable from multi-threaded access
            gDeactivatedEvent.notifyOne ();
        }

        NfcTag::getInstance().setDeactivationState (deactivated);
        nativeNfcTag_resetPresenceCheck();
        NfcTag::getInstance().connectionEventHandler (NFA_DEACTIVATED_EVT, NULL);
        nativeNfcTag_abortWaits();
        NfcTag::getInstance().abort ();

        return JNI_FALSE;
    }

    if (nfcManager_isNfcActive() == false)
    {
        ALOGD ("%s: NFC is no longer active.", __func__);
        return JNI_FALSE;
    }

    if (!sRfInterfaceMutex.tryLock())
    {
        ALOGD ("%s: tag is being reSelected assume it is present", __func__);
        return JNI_TRUE;
    }

    sRfInterfaceMutex.unlock();

    if (NfcTag::getInstance ().isActivated () == false)
    {
        ALOGD ("%s: tag already deactivated", __func__);
        return JNI_FALSE;
    }
    if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
    {
        status = EXTNS_MfcPresenceCheck ();
        if (status == NFCSTATUS_SUCCESS)
        {
            return (NFCSTATUS_SUCCESS == EXTNS_GetPresenceCheckStatus ()) ? JNI_TRUE : JNI_FALSE;
        }
    }

    {
        SyncEventGuard guard (sPresenceCheckEvent);
        status = NFA_RwPresenceCheck (NfcTag::getInstance().getPresenceCheckAlgorithm());
        if (status == NFA_STATUS_OK)
        {
            sPresenceCheckEvent.wait ();
            isPresent = sIsTagPresent ? JNI_TRUE : JNI_FALSE;
        }
    }

    if (isPresent == JNI_FALSE)
        ALOGD ("%s: tag absent", __func__);
    return isPresent;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doIsNdefFormatable
**
** Description:     Can tag be formatted to store NDEF message?
**                  e: JVM environment.
**                  o: Java object.
**                  libNfcType: Type of tag.
**                  uidBytes: Tag's unique ID.
**                  pollBytes: Data from activation.
**                  actBytes: Data from activation.
**
** Returns:         True if formattable.
**
*******************************************************************************/
static jboolean nativeNfcTag_doIsNdefFormatable (JNIEnv* e,
        jobject o, jint /*libNfcType*/, jbyteArray, jbyteArray,
        jbyteArray)
{
    jboolean isFormattable = JNI_FALSE;
    tNFC_PROTOCOL protocol = NfcTag::getInstance().getProtocol();
    if (NFA_PROTOCOL_T1T == protocol || NFA_PROTOCOL_ISO15693 == protocol
        || NFA_PROTOCOL_MIFARE == protocol)
    {
        isFormattable = JNI_TRUE;
    }
    else if (NFA_PROTOCOL_T3T == protocol)
    {
        isFormattable = NfcTag::getInstance().isFelicaLite() ? JNI_TRUE : JNI_FALSE;
    }
    else if (NFA_PROTOCOL_T2T == protocol)
    {
        isFormattable = ( NfcTag::getInstance().isMifareUltralight() |
                          NfcTag::getInstance().isInfineonMyDMove() |
                          NfcTag::getInstance().isKovioType2Tag() )
                        ? JNI_TRUE : JNI_FALSE;
    }
    else if (NFA_PROTOCOL_ISO_DEP == protocol)
    {
        /**
         * Determines whether this is a formatable IsoDep tag - currectly only NXP DESFire
         * is supported.
         */
        uint8_t cmd[] = {0x90, 0x60, 0x00, 0x00, 0x00};

        if (NfcTag::getInstance ().isMifareDESFire ())
        {
            /* Identifies as DESfire, use get version cmd to be sure */
            jbyteArray versionCmd = e->NewByteArray (5);
            e->SetByteArrayRegion (versionCmd, 0, 5, (jbyte*) cmd);
            jbyteArray respBytes = nativeNfcTag_doTransceive(e, o, versionCmd, JNI_TRUE, NULL);
            if (respBytes != NULL)
            {
                // Check whether the response matches a typical DESfire
                // response.
                // libNFC even does more advanced checking than we do
                // here, and will only format DESfire's with a certain
                // major/minor sw version and NXP as a manufacturer.
                // We don't want to do such checking here, to avoid
                // having to change code in multiple places.
                // A succesful (wrapped) DESFire getVersion command returns
                // 9 bytes, with byte 7 0x91 and byte 8 having status
                // code 0xAF (these values are fixed and well-known).
                int respLength = e->GetArrayLength (respBytes);
                uint8_t* resp = (uint8_t*) e->GetByteArrayElements (respBytes, NULL);
                if (respLength == 9 && resp[7] == 0x91 && resp[8] == 0xAF)
                {
                    isFormattable = JNI_TRUE;
                }
                e->ReleaseByteArrayElements (respBytes, (jbyte *) resp, JNI_ABORT);
            }
        }
    }

    ALOGD("%s: is formattable=%u", __func__, isFormattable);
    return isFormattable;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doIsIsoDepNdefFormatable
**
** Description:     Is ISO-DEP tag formattable?
**                  e: JVM environment.
**                  o: Java object.
**                  pollBytes: Data from activation.
**                  actBytes: Data from activation.
**
** Returns:         True if formattable.
**
*******************************************************************************/
static jboolean nativeNfcTag_doIsIsoDepNdefFormatable (JNIEnv *e, jobject o, jbyteArray pollBytes, jbyteArray actBytes)
{
    uint8_t uidFake[] = { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };
    ALOGD ("%s", __func__);
    jbyteArray uidArray = e->NewByteArray (8);
    e->SetByteArrayRegion (uidArray, 0, 8, (jbyte*) uidFake);
    return nativeNfcTag_doIsNdefFormatable (e, o, 0, uidArray, pollBytes, actBytes);
}


/*******************************************************************************
**
** Function:        nativeNfcTag_makeMifareNdefFormat
**
** Description:     Format a mifare classic tag so it can store NDEF message.
**                  e: JVM environment.
**                  o: Java object.
**                  key: Key to acces tag.
**                  keySize: size of Key.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean nativeNfcTag_makeMifareNdefFormat (JNIEnv *e, jobject o, uint8_t *key, uint32_t keySize)
{
    ALOGD ("%s: enter", __func__);
    tNFA_STATUS status = NFA_STATUS_OK;

    status = nativeNfcTag_doReconnect (e, o);
    if (status != NFA_STATUS_OK)
    {
        ALOGD ("%s: reconnect error, status=%u", __func__, status);
        return JNI_FALSE;
    }

    sem_init (&sFormatSem, 0, 0);
    sFormatOk = false;

    status = EXTNS_MfcFormatTag (key, keySize);

    if (status == NFA_STATUS_OK)
    {
        ALOGD ("%s: wait for completion", __func__);
        sem_wait (&sFormatSem);
        status = sFormatOk ? NFA_STATUS_OK : NFA_STATUS_FAILED;
    }
    else
    {
        ALOGE ("%s: error status=%u", __func__, status);
    }

    sem_destroy (&sFormatSem);
    ALOGD ("%s: exit", __func__);
    return (status == NFA_STATUS_OK) ? JNI_TRUE : JNI_FALSE;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doNdefFormat
**
** Description:     Format a tag so it can store NDEF message.
**                  e: JVM environment.
**                  o: Java object.
**                  key: Not used.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean nativeNfcTag_doNdefFormat (JNIEnv *e, jobject o, jbyteArray)
{
    ALOGD ("%s: enter", __func__);
    tNFA_STATUS status = NFA_STATUS_OK;

    // Do not try to format if tag is already deactivated.
    if (NfcTag::getInstance ().isActivated () == false)
    {
        ALOGD ("%s: tag already deactivated(no need to format)", __func__);
        return JNI_FALSE;
    }

    if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
    {
        static uint8_t mfc_key1[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        static uint8_t mfc_key2[6] = {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7};
        jboolean result;

        result = nativeNfcTag_makeMifareNdefFormat (e, o, mfc_key1, sizeof(mfc_key1));
        if (result == JNI_FALSE)
        {
            result = nativeNfcTag_makeMifareNdefFormat (e, o, mfc_key2, sizeof(mfc_key2));
        }
        return result;
    }

    sem_init (&sFormatSem, 0, 0);
    sFormatOk = false;
    status = NFA_RwFormatTag ();
    if (status == NFA_STATUS_OK)
    {
        ALOGD ("%s: wait for completion", __func__);
        sem_wait (&sFormatSem);
        status = sFormatOk ? NFA_STATUS_OK : NFA_STATUS_FAILED;
    }
    else
        ALOGE ("%s: error status=%u", __func__, status);
    sem_destroy (&sFormatSem);

    if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_ISO_DEP)
    {
        int retCode = NFCSTATUS_SUCCESS;
        retCode = nativeNfcTag_doReconnect (e, o);
    }
    ALOGD ("%s: exit", __func__);
    return (status == NFA_STATUS_OK) ? JNI_TRUE : JNI_FALSE;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doMakeReadonlyResult
**
** Description:     Receive the result of making a tag read-only. Called by the
**                  NFA_SET_TAG_RO_EVT.
**                  status: Status of the operation.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_doMakeReadonlyResult (tNFA_STATUS status)
{
    if (sMakeReadonlyWaitingForComplete != JNI_FALSE)
    {
        sMakeReadonlyWaitingForComplete = JNI_FALSE;
        sMakeReadonlyStatus = status;

        sem_post (&sMakeReadonlySem);
    }
}


/*******************************************************************************
**
** Function:        nativeNfcTag_makeMifareReadonly
**
** Description:     Make the mifare classic tag read-only.
**                  e: JVM environment.
**                  o: Java object.
**                  key: Key to access the tag.
**                  keySize: size of Key.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean nativeNfcTag_makeMifareReadonly (JNIEnv *e, jobject o, uint8_t *key, int32_t keySize)
{
    jboolean result = JNI_FALSE;
    tNFA_STATUS status = NFA_STATUS_OK;

    sMakeReadonlyStatus = NFA_STATUS_FAILED;

    ALOGD ("%s", __func__);

    /* Create the make_readonly semaphore */
    if (sem_init (&sMakeReadonlySem, 0, 0) == -1)
    {
        ALOGE ("%s: Make readonly semaphore creation failed (errno=0x%08x)", __func__, errno);
        return JNI_FALSE;
    }

    sMakeReadonlyWaitingForComplete = JNI_TRUE;

    status = nativeNfcTag_doReconnect (e, o);
    if (status != NFA_STATUS_OK)
    {
        goto TheEnd;
    }

    status = EXTNS_MfcSetReadOnly (key, keySize);
    if (status != NFA_STATUS_OK)
    {
        goto TheEnd;
    }
    sem_wait (&sMakeReadonlySem);

    if (sMakeReadonlyStatus == NFA_STATUS_OK)
    {
        result = JNI_TRUE;
    }

TheEnd:
    /* Destroy semaphore */
    if (sem_destroy (&sMakeReadonlySem))
    {
        ALOGE ("%s: Failed to destroy read_only semaphore (errno=0x%08x)", __func__, errno);
    }
    sMakeReadonlyWaitingForComplete = JNI_FALSE;
    return result;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_doMakeReadonly
**
** Description:     Make the tag read-only.
**                  e: JVM environment.
**                  o: Java object.
**                  key: Key to access the tag.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean nativeNfcTag_doMakeReadonly (JNIEnv *e, jobject o, jbyteArray)
{
    jboolean result = JNI_FALSE;
    tNFA_STATUS status;

    ALOGD ("%s", __func__);

    if (sCurrentConnectedTargetProtocol == NFA_PROTOCOL_MIFARE)
    {
        static uint8_t mfc_key1[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        static uint8_t mfc_key2[6] = {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7};
        result = nativeNfcTag_makeMifareReadonly (e, o, mfc_key1, sizeof(mfc_key1));
        if (result == JNI_FALSE)
        {
            result = nativeNfcTag_makeMifareReadonly (e, o, mfc_key2, sizeof(mfc_key2));
        }
        return result;
    }

    /* Create the make_readonly semaphore */
    if (sem_init (&sMakeReadonlySem, 0, 0) == -1)
    {
        ALOGE ("%s: Make readonly semaphore creation failed (errno=0x%08x)", __func__, errno);
        return JNI_FALSE;
    }

    sMakeReadonlyWaitingForComplete = JNI_TRUE;

    // Hard-lock the tag (cannot be reverted)
    status = NFA_RwSetTagReadOnly(TRUE);
    if (status == NFA_STATUS_REJECTED)
    {
        status = NFA_RwSetTagReadOnly (FALSE); //try soft lock
        if (status != NFA_STATUS_OK)
        {
            ALOGE ("%s: fail soft lock, status=%d", __func__, status);
            goto TheEnd;
        }
    }
    else if (status != NFA_STATUS_OK)
    {
        ALOGE ("%s: fail hard lock, status=%d", __func__, status);
        goto TheEnd;
    }

    /* Wait for check NDEF completion status */
    if (sem_wait (&sMakeReadonlySem))
    {
        ALOGE ("%s: Failed to wait for make_readonly semaphore (errno=0x%08x)", __func__, errno);
        goto TheEnd;
    }

    if (sMakeReadonlyStatus == NFA_STATUS_OK)
    {
        result = JNI_TRUE;
    }

TheEnd:
    /* Destroy semaphore */
    if (sem_destroy (&sMakeReadonlySem))
    {
        ALOGE ("%s: Failed to destroy read_only semaphore (errno=0x%08x)", __func__, errno);
    }
    sMakeReadonlyWaitingForComplete = JNI_FALSE;
    return result;
}


/*******************************************************************************
**
** Function:        nativeNfcTag_registerNdefTypeHandler
**
** Description:     Register a callback to receive NDEF message from the tag
**                  from the NFA_NDEF_DATA_EVT.
**
** Returns:         None
**
*******************************************************************************/
//register a callback to receive NDEF message from the tag
//from the NFA_NDEF_DATA_EVT;
void nativeNfcTag_registerNdefTypeHandler ()
{
    ALOGD ("%s", __func__);
    sNdefTypeHandlerHandle = NFA_HANDLE_INVALID;
    NFA_RegisterNDefTypeHandler (TRUE, NFA_TNF_DEFAULT, (uint8_t *) "", 0, ndefHandlerCallback);
    EXTNS_MfcRegisterNDefTypeHandler (ndefHandlerCallback);
}


/*******************************************************************************
**
** Function:        nativeNfcTag_deregisterNdefTypeHandler
**
** Description:     No longer need to receive NDEF message from the tag.
**
** Returns:         None
**
*******************************************************************************/
void nativeNfcTag_deregisterNdefTypeHandler ()
{
    ALOGD ("%s", __func__);
    NFA_DeregisterNDefTypeHandler (sNdefTypeHandlerHandle);
    sNdefTypeHandlerHandle = NFA_HANDLE_INVALID;
}


/*****************************************************************************
**
** JNI functions for Android 4.0.3
**
*****************************************************************************/
static JNINativeMethod gMethods[] =
{
   {"doConnect", "(I)I", (void *)nativeNfcTag_doConnect},
   {"doDisconnect", "()Z", (void *)nativeNfcTag_doDisconnect},
   {"doReconnect", "()I", (void *)nativeNfcTag_doReconnect},
   {"doHandleReconnect", "(I)I", (void *)nativeNfcTag_doHandleReconnect},
   {"doTransceive", "([BZ[I)[B", (void *)nativeNfcTag_doTransceive},
   {"doGetNdefType", "(II)I", (void *)nativeNfcTag_doGetNdefType},
   {"doCheckNdef", "([I)I", (void *)nativeNfcTag_doCheckNdef},
   {"doRead", "()[B", (void *)nativeNfcTag_doRead},
   {"doWrite", "([B)Z", (void *)nativeNfcTag_doWrite},
   {"doPresenceCheck", "()Z", (void *)nativeNfcTag_doPresenceCheck},
   {"doIsIsoDepNdefFormatable", "([B[B)Z", (void *)nativeNfcTag_doIsIsoDepNdefFormatable},
   {"doNdefFormat", "([B)Z", (void *)nativeNfcTag_doNdefFormat},
   {"doMakeReadonly", "([B)Z", (void *)nativeNfcTag_doMakeReadonly},
};


/*******************************************************************************
**
** Function:        register_com_android_nfc_NativeNfcTag
**
** Description:     Regisgter JNI functions with Java Virtual Machine.
**                  e: Environment of JVM.
**
** Returns:         Status of registration.
**
*******************************************************************************/
int register_com_android_nfc_NativeNfcTag (JNIEnv *e)
{
    ALOGD ("%s", __func__);
    return jniRegisterNativeMethods (e, gNativeNfcTagClassName, gMethods, NELEM (gMethods));
}


} /* namespace android */
