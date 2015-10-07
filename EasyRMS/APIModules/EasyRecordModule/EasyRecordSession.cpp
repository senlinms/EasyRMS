/*
	Copyright (c) 2013-2015 EasyDarwin.ORG.  All rights reserved.
	Github: https://github.com/EasyDarwin
	WEChat: EasyDarwin
	Website: http://www.easydarwin.org
*/
/*
    File:       EasyRecordSession.cpp
    Contains:   Implementation of object defined in EasyRecordSession.h. 
*/
#include "EasyRecordSession.h"
#include "SocketUtils.h"
#include "EventContext.h"
#include "OSMemory.h"
#include "OS.h"
#include "atomic.h"
#include "QTSSModuleUtils.h"
#include <errno.h>
#include "QTSServerInterface.h"

#ifndef __Win32__
    #include <unistd.h>
#endif

// PREFS
static UInt32                   sDefaultM3U8Version					= 3; 
static Bool16                   sDefaultAllowCache					= false; 
static UInt32                   sDefaultTargetDuration				= 4;
static UInt32                   sDefaultPlaylistCapacity			= 4;
static char*					sDefaultHTTPRootDir					= "http://www.easydarwin.org/";

UInt32                          EasyRecordSession::sM3U8Version		= 3;
Bool16                          EasyRecordSession::sAllowCache			= false;
UInt32                          EasyRecordSession::sTargetDuration		= 4;
UInt32                          EasyRecordSession::sPlaylistCapacity	= 4;
char*							EasyRecordSession::sHTTPRootDir		= NULL;

void EasyRecordSession::Initialize(QTSS_ModulePrefsObject inPrefs)
{
	delete [] sHTTPRootDir;
    sHTTPRootDir = QTSSModuleUtils::GetStringAttribute(inPrefs, "HTTP_ROOT_DIR", sDefaultHTTPRootDir);

	QTSSModuleUtils::GetAttribute(inPrefs, "M3U8_VERSION", qtssAttrDataTypeUInt32,
							  &EasyRecordSession::sM3U8Version, &sDefaultM3U8Version, sizeof(sDefaultM3U8Version));

	QTSSModuleUtils::GetAttribute(inPrefs, "ALLOW_CACHE", qtssAttrDataTypeBool16,
							  &EasyRecordSession::sAllowCache, &sDefaultAllowCache, sizeof(sDefaultAllowCache));

	QTSSModuleUtils::GetAttribute(inPrefs, "TARGET_DURATION", qtssAttrDataTypeUInt32,
							  &EasyRecordSession::sTargetDuration, &sDefaultTargetDuration, sizeof(sDefaultTargetDuration));

	QTSSModuleUtils::GetAttribute(inPrefs, "PLAYLIST_CAPACITY", qtssAttrDataTypeUInt32,
							  &EasyRecordSession::sPlaylistCapacity, &sDefaultPlaylistCapacity, sizeof(sDefaultPlaylistCapacity));

}

/* RTSPClient获取数据后回调给上层 */
int Easy_APICALL __RTSPClientCallBack( int _chid, int *_chPtr, int _mediatype, char *pbuf, RTSP_FRAME_INFO *frameinfo)
{
	EasyRecordSession* pHLSSession = (EasyRecordSession *)_chPtr;

	if (NULL == pHLSSession)	return -1;

	//投递到具体对应的EasyRecordSession进行处理
	pHLSSession->ProcessData(_chid, _mediatype, pbuf, frameinfo);

	return 0;
}

EasyRecordSession::EasyRecordSession(StrPtrLen* inSessionID)
:   fQueueElem(),
	fRTSPClientHandle(NULL),
	fHLSHandle(NULL),
	tsTimeStampMSsec(0),
	fPlayTime(0),
    fTotalPlayTime(0),
	fLastStatPlayTime(0),
	fLastStatBitrate(0),
	fNumPacketsReceived(0),
	fLastNumPacketsReceived(0),
	fNumBytesReceived(0),
	fLastNumBytesReceived(0),
	fTimeoutTask(NULL, 60*1000)
{
    fTimeoutTask.SetTask(this);
    fQueueElem.SetEnclosingObject(this);

    if (inSessionID != NULL)
    {
        fHLSSessionID.Ptr = NEW char[inSessionID->Len + 1];
        ::memcpy(fHLSSessionID.Ptr, inSessionID->Ptr, inSessionID->Len);
		fHLSSessionID.Ptr[inSessionID->Len] = '\0';
        fHLSSessionID.Len = inSessionID->Len;
        fRef.Set(fHLSSessionID, this);
    }

	fHLSURL[0] = '\0';
	fSourceURL[0] = '\0';

	this->Signal(Task::kStartEvent);
}


EasyRecordSession::~EasyRecordSession()
{
	HLSSessionRelease();
    fHLSSessionID.Delete();

    if (this->GetRef()->GetRefCount() == 0)
    {   
        qtss_printf("EasyRecordSession::~EasyRecordSession() UnRegister and delete session =%p refcount=%"_U32BITARG_"\n", GetRef(), GetRef()->GetRefCount() ) ;       
        QTSServerInterface::GetServer()->GetRecordSessionMap()->UnRegister(GetRef());
    }
}

SInt64 EasyRecordSession::Run()
{
    EventFlags theEvents = this->GetEvents();
	OSRefTable* sHLSSessionMap =  QTSServerInterface::GetServer()->GetRecordSessionMap();
	OSMutexLocker locker (sHLSSessionMap->GetMutex());

	if (theEvents & Task::kKillEvent)
    {
        return -1;
    }

	if (theEvents & Task::kTimeoutEvent)
    {
		char msgStr[2048] = { 0 };
		qtss_snprintf(msgStr, sizeof(msgStr), "EasyRecordSession::Run Timeout SessionID=%s", fHLSSessionID.Ptr);
		QTSServerInterface::LogError(qtssMessageVerbosity, msgStr);

		return -1;
    }

	//统计数据
	{
		SInt64 curTime = OS::Milliseconds();

		UInt64 bytesReceived = fNumBytesReceived - fLastNumBytesReceived;
		UInt64 durationTime	= curTime - fLastStatPlayTime;

		if(durationTime)
			fLastStatBitrate = (bytesReceived*1000)/(durationTime);

		fLastNumBytesReceived = fNumBytesReceived;
		fLastStatPlayTime = curTime;

	}

    return 2000;
}

QTSS_Error EasyRecordSession::ProcessData(int _chid, int mediatype, char *pbuf, RTSP_FRAME_INFO *frameinfo)
{
	if(NULL == fHLSHandle) return QTSS_Unimplemented;

	if ((mediatype == EASY_SDK_VIDEO_FRAME_FLAG) || (mediatype == EASY_SDK_AUDIO_FRAME_FLAG))
	{
		fNumPacketsReceived++;
		fNumBytesReceived += frameinfo->length;
	}

	if (mediatype == EASY_SDK_VIDEO_FRAME_FLAG)
	{
		unsigned long long llPTS = (frameinfo->timestamp_sec%1000000)*1000 + frameinfo->timestamp_usec/1000;	

		printf("Get %s Video \tLen:%d \ttm:%u.%u \t%u\n",frameinfo->type==EASY_SDK_VIDEO_FRAME_I?"I":"P", frameinfo->length, frameinfo->timestamp_sec, frameinfo->timestamp_usec, llPTS);

		unsigned int uiFrameType = 0;
		if (frameinfo->type == EASY_SDK_VIDEO_FRAME_I)
		{
			uiFrameType = TS_TYPE_PES_VIDEO_I_FRAME;
		}
		else if (frameinfo->type == EASY_SDK_VIDEO_FRAME_P)
		{
			uiFrameType = TS_TYPE_PES_VIDEO_P_FRAME;
		}
		else
		{
			return QTSS_OutOfState;
		}

		EasyHLS_VideoMux(fHLSHandle, uiFrameType, (unsigned char*)pbuf, frameinfo->length, llPTS*90, llPTS*90, llPTS*90);
	}
	else if (mediatype == EASY_SDK_AUDIO_FRAME_FLAG)
	{

		unsigned long long llPTS = (frameinfo->timestamp_sec%1000000)*1000 + frameinfo->timestamp_usec/1000;	

		printf("Get Audio \tLen:%d \ttm:%u.%u \t%u\n", frameinfo->length, frameinfo->timestamp_sec, frameinfo->timestamp_usec, llPTS);

		if (frameinfo->codec == EASY_SDK_AUDIO_CODEC_AAC)
		{
			EasyHLS_AudioMux(fHLSHandle, (unsigned char*)pbuf, frameinfo->length, llPTS*90, llPTS*90);
		}
	}
	else if (mediatype == EASY_SDK_EVENT_FRAME_FLAG)
	{
		if (NULL == pbuf && NULL == frameinfo)
		{
			printf("Connecting:%s ...\n", fHLSSessionID.Ptr);
		}
		else if (NULL!=frameinfo && frameinfo->type==0xF1)
		{
			printf("Lose Packet:%s ...\n", fHLSSessionID.Ptr);
		}
	}

	return QTSS_NoErr;
}

/*
	创建HLS直播Session
*/
QTSS_Error	EasyRecordSession::HLSSessionStart(char* rtspUrl, UInt32 inTimeout)
{
	QTSS_Error theErr = QTSS_NoErr;

	do{
		if(NULL == fRTSPClientHandle)
		{
			//创建RTSPClient
			EasyRTSP_Init(&fRTSPClientHandle);

			if (NULL == fRTSPClientHandle)
			{
				theErr = QTSS_RequestFailed;
				break;
			}

			::sprintf(fSourceURL, "%s", rtspUrl);

			unsigned int mediaType = EASY_SDK_VIDEO_FRAME_FLAG | EASY_SDK_AUDIO_FRAME_FLAG;

			EasyRTSP_SetCallback(fRTSPClientHandle, __RTSPClientCallBack);
			EasyRTSP_OpenStream(fRTSPClientHandle, 0, rtspUrl,RTP_OVER_TCP, mediaType, 0, 0, this, 1000, 0);

			fPlayTime = fLastStatPlayTime = OS::Milliseconds();
			fNumPacketsReceived = fLastNumPacketsReceived = 0;
			fNumBytesReceived = fLastNumBytesReceived = 0;

		}

		if(NULL == fHLSHandle)
		{
			//创建HLSSessioin Sink
			fHLSHandle = EasyHLS_Session_Create(sPlaylistCapacity, sAllowCache, sM3U8Version);

			if (NULL == fHLSHandle)
			{
				theErr = QTSS_Unimplemented;
				break;
			}

			char subDir[QTSS_MAX_URL_LENGTH] = { 0 };
			qtss_sprintf(subDir,"%s/",fHLSSessionID.Ptr);
			EasyHLS_ResetStreamCache(fHLSHandle, "./Movies/", subDir, fHLSSessionID.Ptr, sTargetDuration);

			char msgStr[2048] = { 0 };
			qtss_snprintf(msgStr, sizeof(msgStr), "EasyRecordSession::EasyHLS_ResetStreamCache SessionID=%s,movieFolder=%s,subDir=%s", fHLSSessionID.Ptr, "./Movies/", subDir);
			QTSServerInterface::LogError(qtssMessageVerbosity, msgStr);
					
			qtss_sprintf(fHLSURL, "%s%s/%s.m3u8", sHTTPRootDir, fHLSSessionID.Ptr, fHLSSessionID.Ptr);
		}
		
		fTimeoutTask.SetTimeout(inTimeout * 1000);
	}while(0);

	char msgStr[2048] = { 0 };
	qtss_snprintf(msgStr, sizeof(msgStr), "EasyRecordSession::HLSSessionStart SessionID=%s,url=%s,return=%d", fHLSSessionID.Ptr, rtspUrl, theErr);
	QTSServerInterface::LogError(qtssMessageVerbosity, msgStr);

	return theErr;
}

QTSS_Error	EasyRecordSession::HLSSessionRelease()
{
	qtss_printf("HLSSession Release....\n");
	
	//释放source
	if(fRTSPClientHandle)
	{
		EasyRTSP_CloseStream(fRTSPClientHandle);
		EasyRTSP_Deinit(&fRTSPClientHandle);
		fRTSPClientHandle = NULL;
		fSourceURL[0] = '\0';
	}

	//释放sink
	if(fHLSHandle)
	{
		EasyHLS_Session_Release(fHLSHandle);
		fHLSHandle = NULL;
		fHLSURL[0] = '\0';
 	}

	return QTSS_NoErr;
}

char* EasyRecordSession::GetHLSURL()
{
	return fHLSURL;
}

char* EasyRecordSession::GetSourceURL()
{
	return 	fSourceURL;
}