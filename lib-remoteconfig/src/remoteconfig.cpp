/**
 * @file remoteconfig.cpp
 *
 */
/* Copyright (C) 2019 by Arjan van Vught mailto:info@raspberrypi-dmx.nl
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#ifndef ALIGNED
 #define ALIGNED __attribute__ ((aligned (4)))
#endif

#include "remoteconfig.h"

#include "hardware.h"
#include "network.h"
#include "display.h"

#include "spiflashstore.h"

/* rconfig.txt */
#include "remoteconfigparams.h"
#include "storeremoteconfig.h"
/* network.txt */
#include "networkparams.h"
/* artnet.txt */
#include "artnet4params.h"
/* e131.txt */
#include "e131params.h"
#if defined (OSC_SERVER)
 /* osc.txt */
 #include "oscserverparms.h"
 #include "storeoscserver.h"
#endif
/* params.txt */
#include "dmxparams.h"
/* devices.txt */
#include "tlc59711dmxparams.h"
#include "ws28xxdmxparams.h"
#if defined (LTC_READER)
 /* ltc.txt */
 #include "ltcparams.h"
 #include "storeltc.h"
 /* tcnet.txt */
 #include "tcnetparams.h"
 #include "storetcnet.h"
#endif

// nuc-i5:~/uboot-spi/u-boot$ grep CONFIG_BOOTCOMMAND include/configs/sunxi-common.h
// #define CONFIG_BOOTCOMMAND "sf probe; sf read 40000000 180000 22000; bootm 40000000"
#define FIRMWARE_MAX_SIZE	0x22000

#include "tftpfileserver.h"
#include "spiflashinstall.h"

#include "debug.h"

#ifndef MIN
 #define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char sRemoteConfigs[REMOTE_CONFIG_LAST][12] ALIGNED = { "Art-Net", "sACN E1.31", "OSC", "LTC" };
static const char sRemoteConfigModes[REMOTE_CONFIG_MODE_LAST][9] ALIGNED = { "DMX", "RDM", "Monitor", "Pixel", "TimeCode" };

static const char sRequestReboot[] ALIGNED = "?reboot##";
#define REQUEST_REBOOT_LENGTH (sizeof(sRequestReboot)/sizeof(sRequestReboot[0]) - 1)

static const char sRequestList[] ALIGNED = "?list#";
#define REQUEST_LIST_LENGTH (sizeof(sRequestList)/sizeof(sRequestList[0]) - 1)

static const char sRequestGet[] ALIGNED = "?get#";
#define REQUEST_GET_LENGTH (sizeof(sRequestGet)/sizeof(sRequestGet[0]) - 1)

static const char sRequestUptime[] ALIGNED = "?uptime#";
#define REQUEST_UPTIME_LENGTH (sizeof(sRequestUptime)/sizeof(sRequestUptime[0]) - 1)

static const char sRequestStore[] ALIGNED = "?store#";
#define REQUEST_STORE_LENGTH (sizeof(sRequestStore)/sizeof(sRequestStore[0]) - 1)

static const char sGetDisplay[] ALIGNED = "?display#";
#define GET_DISPLAY_LENGTH (sizeof(sGetDisplay)/sizeof(sGetDisplay[0]) - 1)

static const char sSetDisplay[] ALIGNED = "!display#";
#define SET_DISPLAY_LENGTH (sizeof(sSetDisplay)/sizeof(sSetDisplay[0]) - 1)

static const char sGetTFTP[] ALIGNED = "?tftp#";
#define GET_TFTP_LENGTH (sizeof(sGetTFTP)/sizeof(sGetTFTP[0]) - 1)

static const char sSetTFTP[] ALIGNED = "!tftp#";
#define SET_TFTP_LENGTH (sizeof(sSetTFTP)/sizeof(sSetTFTP[0]) - 1)

enum TTxtFile {
	TXT_FILE_RCONFIG,
	TXT_FILE_NETWORK,
	TXT_FILE_ARTNET,
	TXT_FILE_E131,
	TXT_FILE_OSC,
	TXT_FILE_PARAMS,
	TXT_FILE_DEVICES,
	TXT_FILE_LTC,
	TXT_FILE_TCNET,
	TXT_FILE_LAST
};

static const char sTxtFile[TXT_FILE_LAST][12] =          { "rconfig.txt", "network.txt", "artnet.txt", "e131.txt", "osc.txt", "params.txt", "devices.txt", "ltc.txt", "tcnet.txt" };
static const uint8_t sTxtFileNameLength[TXT_FILE_LAST] = {  11,            11,            10,           8,          7,         10,           11,           7,         9};
static const TStore sMap[TXT_FILE_LAST] = 				 { STORE_RCONFIG, STORE_NETWORK, STORE_ARTNET, STORE_E131, STORE_OSC, STORE_DMXSEND, STORE_WS28XXDMX, STORE_LTC, STORE_TCNET};

#define UDP_PORT			0x2905
#define UDP_BUFFER_SIZE		512
#define UDP_DATA_MIN_SIZE	MIN(MIN(MIN(MIN(REQUEST_REBOOT_LENGTH, REQUEST_LIST_LENGTH),REQUEST_GET_LENGTH),REQUEST_UPTIME_LENGTH),SET_DISPLAY_LENGTH)

RemoteConfig::RemoteConfig(TRemoteConfig tRemoteConfig, TRemoteConfigMode tRemoteConfigMode, uint8_t nOutputs):
	m_bDisable(false),
	m_bDisableWrite(false),
	m_bEnableReboot(false),
	m_bEnableUptime(false),
	m_bEnableTFTP(false),
	m_pTFTPFileServer(0),
	m_pTFTPBuffer(0),
	m_nIdLength(0),
	m_nHandle(-1),
	m_pUdpBuffer(0),
	m_nIPAddressFrom(0),
	m_nBytesReceived(0)

{
	assert(tRemoteConfig < REMOTE_CONFIG_LAST);
	assert(tRemoteConfigMode < REMOTE_CONFIG_MODE_LAST);

	memset(m_aId, 0, sizeof m_aId);
	memset(&m_tRemoteConfigListBin, 0, sizeof m_tRemoteConfigListBin);

	m_nIdLength = snprintf(m_aId, sizeof(m_aId) - 1, "" IPSTR ",%s,%s,%d\n", IP2STR(Network::Get()->GetIp()), sRemoteConfigs[tRemoteConfig], sRemoteConfigModes[tRemoteConfigMode], nOutputs);

	DEBUG_PRINTF("%d:[%s]", m_nIdLength, m_aId);

	Network::Get()->MacAddressCopyTo(m_tRemoteConfigListBin.aMacAddress);
	m_tRemoteConfigListBin.nType = tRemoteConfig;
	m_tRemoteConfigListBin.nMode = tRemoteConfigMode;
	m_tRemoteConfigListBin.nActiveUniverses = nOutputs;

#ifndef NDEBUG
	debug_dump((void *)&m_tRemoteConfigListBin, sizeof m_tRemoteConfigListBin);
#endif

	m_nHandle = Network::Get()->Begin(UDP_PORT);
	assert(m_nHandle != -1);

	m_pUdpBuffer = new uint8_t[UDP_BUFFER_SIZE];
	assert(m_pUdpBuffer != 0);
}

RemoteConfig::~RemoteConfig(void) {
	delete [] m_pUdpBuffer;
	m_pUdpBuffer = 0;
}

void RemoteConfig::SetDisable(bool bDisable) {
	if (bDisable && !m_bDisable) {
		Network::Get()->End(UDP_PORT);
		m_nHandle = -1;
		m_bDisable = true;
	} else if (!bDisable && m_bDisable) {
		m_nHandle = Network::Get()->Begin(UDP_PORT);
		assert(m_nHandle != -1);
		m_bDisable = false;
	}

	DEBUG_PRINTF("m_bDisable=%d", (int) m_bDisable);
}

void RemoteConfig::SetDisableWrite(bool bDisableWrite) {
	m_bDisableWrite = bDisableWrite;

	DEBUG_PRINTF("m_bDisableWrite=%d", (int) m_bDisableWrite);
}

void RemoteConfig::SetEnableReboot(bool bEnableReboot) {
	m_bEnableReboot = bEnableReboot;

	DEBUG_PRINTF("m_bEnableReboot=%d", (int) m_bEnableReboot);
}

void RemoteConfig::SetEnableUptime(bool bEnableUptime) {
	m_bEnableUptime = bEnableUptime;

	DEBUG_PRINTF("m_bEnableUptime=%d", (int) m_bEnableUptime);
}

void RemoteConfig::SetDisplayName(const char *pDisplayName) {
	DEBUG_ENTRY

	DEBUG_PRINTF("%d:[%s]", m_nIdLength, m_aId);

	// BUG there is NO sanity check for adding multiple times
	uint32_t nSize = strlen(pDisplayName);

	if ((m_nIdLength + nSize + 2) < REMOTE_CONFIG_ID_LENGTH) {
		m_aId[m_nIdLength - 1] = ',';
		strcpy((char *) &m_aId[m_nIdLength], pDisplayName);
		m_aId[m_nIdLength + nSize] = '\n';
	}

	m_nIdLength += (nSize + 1);

	DEBUG_PRINTF("%d:[%s]", m_nIdLength, m_aId);

	strncpy((char *)m_tRemoteConfigListBin.aDisplayName, pDisplayName, REMOTE_CONFIG_DISPLAY_NAME_LENGTH);
#ifndef NDEBUG
	debug_dump((void *)&m_tRemoteConfigListBin, sizeof m_tRemoteConfigListBin);
#endif

	DEBUG_EXIT
}

int RemoteConfig::Run(void) {
	uint16_t nForeignPort;

	if (__builtin_expect((m_bDisable), 1)) {
		return 0;
	}

	if (__builtin_expect((m_pTFTPFileServer != 0), 0)) {
		m_pTFTPFileServer->Run();
	}

	m_nBytesReceived = Network::Get()->RecvFrom(m_nHandle, m_pUdpBuffer, (uint16_t) UDP_BUFFER_SIZE, &m_nIPAddressFrom, &nForeignPort);

	if (__builtin_expect((m_nBytesReceived < (int) UDP_DATA_MIN_SIZE), 1)) {
		return 0;
	}

#ifndef NDEBUG
	debug_dump((void *)m_pUdpBuffer, m_nBytesReceived);
#endif

	if (m_pUdpBuffer[m_nBytesReceived - 1] == '\n') {
		DEBUG_PUTS("\'\\n\'");
		m_nBytesReceived--;
	}

	if (m_pUdpBuffer[0] == '?') {
		DEBUG_PUTS("?");
		if ((m_bEnableReboot) && (memcmp(m_pUdpBuffer, sRequestReboot, REQUEST_REBOOT_LENGTH) == 0)) {
			HandleReboot();
		} else if ((m_bEnableUptime) && (memcmp(m_pUdpBuffer, sRequestUptime, REQUEST_UPTIME_LENGTH) == 0)) {
			HandleUptime();
		} else if (memcmp(m_pUdpBuffer, sRequestList, REQUEST_LIST_LENGTH) == 0) {
			HandleList();
		} else if ((m_nBytesReceived > REQUEST_GET_LENGTH) && (memcmp(m_pUdpBuffer, sRequestGet, REQUEST_GET_LENGTH) == 0)) {
			HandleGet();
		} else if ((m_nBytesReceived > REQUEST_STORE_LENGTH) && (memcmp(m_pUdpBuffer, sRequestStore, REQUEST_STORE_LENGTH) == 0)) {
			HandleStoreGet();
		} else if ((m_nBytesReceived >= GET_DISPLAY_LENGTH) && (memcmp(m_pUdpBuffer, sGetDisplay, GET_DISPLAY_LENGTH) == 0)) {
			HandleDisplayGet();
		} else if ((m_nBytesReceived >= GET_TFTP_LENGTH) && (memcmp(m_pUdpBuffer, sGetTFTP, GET_TFTP_LENGTH) == 0)) {
			HandleTftpGet();
		} else {
#ifndef NDEBUG
			Network::Get()->SendTo(m_nHandle, (const uint8_t *)"?#ERROR#\n", 9, m_nIPAddressFrom, (uint16_t) UDP_PORT);
#endif
		}
	} else if ((!m_bDisableWrite) && (m_pUdpBuffer[0] == '#')) {
		DEBUG_PUTS("#");
		HandleTxtFile();
	} else if (m_pUdpBuffer[0] == '!') {
		DEBUG_PUTS("!");
		if ((m_nBytesReceived >= (SET_DISPLAY_LENGTH)) && (memcmp(m_pUdpBuffer, sSetDisplay, SET_DISPLAY_LENGTH) == 0)) {
			DEBUG_PUTS(sSetDisplay);
			HandleDisplaySet();
		} else if ((m_nBytesReceived >= (GET_TFTP_LENGTH)) && (memcmp(m_pUdpBuffer, sSetTFTP, GET_TFTP_LENGTH) == 0)) {
			DEBUG_PUTS(sSetTFTP);
			HandleTftpSet();
		} else {
#ifndef NDEBUG
			Network::Get()->SendTo(m_nHandle, (const uint8_t *)"!#ERROR#\n", 9, m_nIPAddressFrom, (uint16_t) UDP_PORT);
#endif
		}
	} else {
		return 0;
	}

	return m_nBytesReceived;
}

uint32_t RemoteConfig::GetIndex(const void *p) {
	uint32_t i;

	for (i = 0; i < TXT_FILE_LAST; i++) {
		if (memcmp(p, (const void *) sTxtFile[i], sTxtFileNameLength[i]) == 0) {
			break;
		}
	}

	DEBUG_PRINTF("i=%d", i);

	return i;
}

void RemoteConfig::HandleReboot(void) {
	DEBUG_ENTRY

	while (SpiFlashStore::Get()->Flash())
		;

	printf("Rebooting ...\n");

	Display::Get()->Cls();
	Display::Get()->TextStatus("Rebooting ...", DISPLAY_7SEGMENT_MSG_INFO_REBOOTING);

	Hardware::Get()->Reboot();

	DEBUG_EXIT
}

void RemoteConfig::HandleUptime() {
	DEBUG_ENTRY

	const uint64_t nUptime = Hardware::Get()->GetUpTime();

	if (m_nBytesReceived == REQUEST_UPTIME_LENGTH) {
		const uint32_t nLength = snprintf((char *)m_pUdpBuffer, UDP_BUFFER_SIZE, "uptime:%ds\n", (int) nUptime);
		Network::Get()->SendTo(m_nHandle, (const uint8_t *)m_pUdpBuffer, nLength, m_nIPAddressFrom, (uint16_t) UDP_PORT);
	} else if (m_nBytesReceived == REQUEST_UPTIME_LENGTH + 3) {
		DEBUG_PUTS("Check for \'bin\' parameter");
		if (memcmp((const void *)&	m_pUdpBuffer[REQUEST_UPTIME_LENGTH], "bin", 3) == 0) {
			Network::Get()->SendTo(m_nHandle, (const uint8_t *)&nUptime, sizeof(uint64_t) , m_nIPAddressFrom, (uint16_t) UDP_PORT);
		}
	}

	DEBUG_EXIT
}

void RemoteConfig::HandleList(void) {
	DEBUG_ENTRY

	if (m_nBytesReceived == REQUEST_LIST_LENGTH) {
		Network::Get()->SendTo(m_nHandle, (const uint8_t *) m_aId, (uint16_t) m_nIdLength, m_nIPAddressFrom, (uint16_t) UDP_PORT);
	} else if (m_nBytesReceived == REQUEST_LIST_LENGTH + 3) {
		DEBUG_PUTS("Check for \'bin\' parameter");
		if (memcmp((const void *)&	m_pUdpBuffer[REQUEST_LIST_LENGTH], "bin", 3) == 0) {
			Network::Get()->SendTo(m_nHandle, (const uint8_t *)&m_tRemoteConfigListBin, sizeof(struct TRemoteConfigListBin) , m_nIPAddressFrom, (uint16_t) UDP_PORT);
		}
	}

	DEBUG_EXIT
}

void RemoteConfig::HandleDisplaySet() {
	DEBUG_ENTRY

	DEBUG_PRINTF("%c", m_pUdpBuffer[SET_DISPLAY_LENGTH]);

	Display::Get()->SetSleep(m_pUdpBuffer[SET_DISPLAY_LENGTH] == '0');

	DEBUG_EXIT
}

void RemoteConfig::HandleDisplayGet() {
	DEBUG_ENTRY

	const bool isOn = !(Display::Get()->isSleep());

	if (m_nBytesReceived == GET_DISPLAY_LENGTH) {
		const uint32_t nLength = snprintf((char *)m_pUdpBuffer, UDP_BUFFER_SIZE, "display:%s\n", isOn ? "On" : "Off");
		Network::Get()->SendTo(m_nHandle, (const uint8_t *)m_pUdpBuffer, nLength, m_nIPAddressFrom, (uint16_t) UDP_PORT);
	} else if (m_nBytesReceived == GET_DISPLAY_LENGTH + 3) {
		DEBUG_PUTS("Check for \'bin\' parameter");
		if (memcmp((const void *)&	m_pUdpBuffer[GET_DISPLAY_LENGTH], "bin", 3) == 0) {
			Network::Get()->SendTo(m_nHandle, (const uint8_t *)&isOn, sizeof(bool) , m_nIPAddressFrom, (uint16_t) UDP_PORT);
		}
	}

	DEBUG_EXIT
}

void RemoteConfig::HandleStoreGet(void) {
	DEBUG_ENTRY

	uint32_t nLenght = 0;
	const uint32_t i = GetIndex((void *)&m_pUdpBuffer[REQUEST_STORE_LENGTH]);

	if (i != TXT_FILE_LAST) {
		SpiFlashStore::Get()->CopyTo(sMap[i], m_pUdpBuffer, nLenght);
	} else {
#ifndef NDEBUG
		Network::Get()->SendTo(m_nHandle, (const uint8_t *) "?get#ERROR#\n", 12, m_nIPAddressFrom, (uint16_t) UDP_PORT);
#endif
	}

#ifndef NDEBUG
	debug_dump((void *)m_pUdpBuffer, nLenght);
#endif
	Network::Get()->SendTo(m_nHandle, m_pUdpBuffer, nLenght, m_nIPAddressFrom, (uint16_t) UDP_PORT);

	DEBUG_EXIT
}

void RemoteConfig::HandleGet(void) {
	DEBUG_ENTRY

	uint32_t nSize = 0;

	const uint32_t i = GetIndex((void *)&m_pUdpBuffer[REQUEST_GET_LENGTH]);

	switch (i) {
	case TXT_FILE_RCONFIG:
		HandleGetRconfigTxt(nSize);
		break;
	case TXT_FILE_NETWORK:
		HandleGetNetworkTxt(nSize);
		break;
	case TXT_FILE_ARTNET:
		HandleGetArtnetTxt(nSize);
		break;
	case TXT_FILE_E131:
		HandleGetE131Txt(nSize);
		break;
#if defined (OSC_SERVER)
	case TXT_FILE_OSC:
		HandleGetOscTxt(nSize);
		break;
#endif
	case TXT_FILE_PARAMS:
		HandleGetParamsTxt(nSize);
		break;
	case TXT_FILE_DEVICES:
		HandleGetDevicesTxt(nSize);
		break;
#if defined (LTC_READER)
	case TXT_FILE_LTC:
		HandleGetLtcTxt(nSize);
		break;
	case TXT_FILE_TCNET:
		HandleGetTCNetTxt(nSize);
		break;
#endif
	default:
#ifndef NDEBUG
		Network::Get()->SendTo(m_nHandle, (const uint8_t *) "?get#ERROR#\n", 12, m_nIPAddressFrom, (uint16_t) UDP_PORT);
#endif
		DEBUG_EXIT
		return;
		__builtin_unreachable ();
		break;
	}

#ifndef NDEBUG
	debug_dump((void *)m_pUdpBuffer, nSize);
#endif
	Network::Get()->SendTo(m_nHandle, m_pUdpBuffer, nSize, m_nIPAddressFrom, (uint16_t) UDP_PORT);

	DEBUG_EXIT
}

void RemoteConfig::HandleGetRconfigTxt(uint32_t& nSize) {
	DEBUG_ENTRY

	RemoteConfigParams remoteConfigParams((RemoteConfigParamsStore *) StoreRemoteConfig::Get());
	remoteConfigParams.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);

	DEBUG_EXIT
}

void RemoteConfig::HandleGetNetworkTxt(uint32_t& nSize) {
	DEBUG_ENTRY

	NetworkParams networkParams((NetworkParamsStore *) SpiFlashStore::Get()->GetStoreNetwork());
	networkParams.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);

	DEBUG_EXIT
}

void RemoteConfig::HandleGetArtnetTxt(uint32_t& nSize) {
	DEBUG_ENTRY

	uint32_t nSizeArtNet3 = 0;

	ArtNetParams artnetparams((ArtNetParamsStore *) SpiFlashStore::Get()->GetStoreArtNet());
	artnetparams.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSizeArtNet3);

	DEBUG_PRINTF("nSizeArtNet3=%d", nSizeArtNet3);

	uint32_t nSizeArtNet4 = 0;

	ArtNet4Params artnet4params((ArtNet4ParamsStore *) SpiFlashStore::Get()->GetStoreArtNet4());
	artnet4params.Save(m_pUdpBuffer + nSizeArtNet3, UDP_BUFFER_SIZE - nSizeArtNet3, nSizeArtNet4);

	DEBUG_PRINTF("nSizeArtNet4=%d", nSizeArtNet4);

	nSize = nSizeArtNet3 + nSizeArtNet4;

	DEBUG_EXIT
}

void RemoteConfig::HandleGetE131Txt(uint32_t& nSize) {
	DEBUG_ENTRY

	E131Params e131params((E131ParamsStore *) SpiFlashStore::Get()->GetStoreE131());
	e131params.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);

	DEBUG_EXIT
}

#if defined (OSC_SERVER)
void RemoteConfig::HandleGetOscTxt(uint32_t& nSize) {
	DEBUG_ENTRY

	OSCServerParams oscServerParams((OSCServerParamsStore *)StoreOscServer::Get());
	oscServerParams.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);

	DEBUG_EXIT
}
#endif

void RemoteConfig::HandleGetParamsTxt(uint32_t& nSize) {
	DEBUG_ENTRY

	DMXParams dmxparams((DMXParamsStore *) SpiFlashStore::Get()->GetStoreDmxSend());
	dmxparams.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);

	DEBUG_EXIT
}

void RemoteConfig::HandleGetDevicesTxt(uint32_t& nSize) {
	DEBUG_ENTRY

	bool bIsSetLedType = false;

	TLC59711DmxParams tlc5911params((TLC59711DmxParamsStore *) SpiFlashStore::Get()->GetStoreTLC59711());

	if (tlc5911params.Load()) {
		if ((bIsSetLedType = tlc5911params.IsSetLedType()) == true) {
			tlc5911params.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);
		}
	}

	if (!bIsSetLedType) {
		WS28xxDmxParams ws28xxparms((WS28xxDmxParamsStore *) SpiFlashStore::Get()->GetStoreWS28xxDmx());
		ws28xxparms.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);
	}

	DEBUG_EXIT
}

#if defined (LTC_READER)
void RemoteConfig::HandleGetLtcTxt(uint32_t& nSize) {
	DEBUG_ENTRY

	LtcParams ltcParams((LtcParamsStore *)  StoreLtc::Get());
	ltcParams.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);

	DEBUG_EXIT
}

void RemoteConfig::HandleGetTCNetTxt(uint32_t& nSize) {
	DEBUG_ENTRY

	TCNetParams tcnetParams((TCNetParamsStore *)  StoreTCNet::Get());
	tcnetParams.Save(m_pUdpBuffer, UDP_BUFFER_SIZE, nSize);

	DEBUG_EXIT
}
#endif

void RemoteConfig::HandleTxtFile(void) {
	DEBUG_ENTRY

	const uint32_t i = GetIndex((void *)&m_pUdpBuffer[1]);

	switch (i) {
	case TXT_FILE_RCONFIG:
		HandleTxtFileRconfig();
		break;
	case TXT_FILE_NETWORK:
		HandleTxtFileNetwork();
		break;
	case TXT_FILE_ARTNET:
		HandleTxtFileArtnet();
		break;
	case TXT_FILE_E131:
		HandleTxtFileE131();
		break;
#if defined (OSC_SERVER)
	case TXT_FILE_OSC:
		HandleTxtFileOsc();
		break;
#endif
	case TXT_FILE_PARAMS:
		HandleTxtFileParams();
		break;
	case TXT_FILE_DEVICES:
		HandleTxtFileDevices();
		break;
#if defined (LTC_READER)
	case TXT_FILE_LTC:
		HandleTxtFileLtc();
		break;

	case TXT_FILE_TCNET:
		HandleTxtFileTCNet();
		break;
#endif
	default:
		break;
	}

	DEBUG_EXIT
}

void RemoteConfig::HandleTxtFileRconfig(void) {
	DEBUG_ENTRY

	RemoteConfigParams remoteConfigParams((RemoteConfigParamsStore *) StoreRemoteConfig::Get());
	remoteConfigParams.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	remoteConfigParams.Dump();
#endif

	DEBUG_EXIT
}

void RemoteConfig::HandleTxtFileNetwork(void) {
	DEBUG_ENTRY

	NetworkParams params((NetworkParamsStore *) SpiFlashStore::Get()->GetStoreNetwork());
	params.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	params.Dump();
#endif

	DEBUG_EXIT
}

void RemoteConfig::HandleTxtFileArtnet(void) {
	DEBUG_ENTRY

	ArtNet4Params artnet4params((ArtNet4ParamsStore *) SpiFlashStore::Get()->GetStoreArtNet4());
	artnet4params.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	artnet4params.Dump();
#endif

	DEBUG_EXIT
}

void RemoteConfig::HandleTxtFileE131(void) {
	DEBUG_ENTRY

	E131Params e131params((E131ParamsStore *) SpiFlashStore::Get()->GetStoreE131());
	e131params.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	e131params.Dump();
#endif
	DEBUG_EXIT
}

#if defined (OSC_SERVER)
void RemoteConfig::HandleTxtFileOsc(void) {
	DEBUG_ENTRY

	OSCServerParams oscServerParams((OSCServerParamsStore *)StoreOscServer::Get());
	oscServerParams.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	oscServerParams.Dump();
#endif

	DEBUG_EXIT
}
#endif

void RemoteConfig::HandleTxtFileParams(void) {
	DEBUG_ENTRY

	DMXParams dmxparams((DMXParamsStore *) SpiFlashStore::Get()->GetStoreDmxSend());
	dmxparams.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	dmxparams.Dump();
#endif

	DEBUG_EXIT
}

void RemoteConfig::HandleTxtFileDevices(void) {
	DEBUG_ENTRY

	TLC59711DmxParams tlc59711params((TLC59711DmxParamsStore *) SpiFlashStore::Get()->GetStoreTLC59711());
	tlc59711params.Load((const char *)m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	tlc59711params.Dump();
#endif
	DEBUG_PRINTF("tlc5911params.IsSetLedType()=%d", tlc59711params.IsSetLedType());

	if (!tlc59711params.IsSetLedType()) {
		WS28xxDmxParams ws28xxparms((WS28xxDmxParamsStore *) SpiFlashStore::Get()->GetStoreWS28xxDmx());
		ws28xxparms.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
		ws28xxparms.Dump();
#endif
	}

	DEBUG_EXIT
}

#if defined (LTC_READER)
void RemoteConfig::HandleTxtFileLtc(void) {
	DEBUG_ENTRY

	LtcParams ltcParams((LtcParamsStore *) StoreLtc::Get());
	ltcParams.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	ltcParams.Dump();
#endif

	DEBUG_EXIT
}

void RemoteConfig::HandleTxtFileTCNet(void) {
	DEBUG_ENTRY

	TCNetParams tcnetParams((TCNetParamsStore *) StoreTCNet::Get());
	tcnetParams.Load((const char *) m_pUdpBuffer, m_nBytesReceived);
#ifndef NDEBUG
	tcnetParams.Dump();
#endif

	DEBUG_EXIT
}
#endif

/**
 * TFTP Update firmware
 */

void RemoteConfig::HandleTftpSet(void) {
	DEBUG_ENTRY

	DEBUG_PRINTF("%c", m_pUdpBuffer[GET_TFTP_LENGTH]);

	m_bEnableTFTP = (m_pUdpBuffer[GET_TFTP_LENGTH] != '0');

	if (m_bEnableTFTP && (m_pTFTPFileServer == 0)) {
		DEBUG_PUTS("Create TFTP Server");

		m_pTFTPBuffer = new uint8_t[FIRMWARE_MAX_SIZE];
		assert(m_pTFTPBuffer != 0);

		m_pTFTPFileServer = new TFTPFileServer(m_pTFTPBuffer, FIRMWARE_MAX_SIZE);
		assert(m_pTFTPFileServer != 0);
		Display::Get()->Status(DISPLAY_7SEGMENT_MSG_INFO_TFTP_ON);
	} else if (!m_bEnableTFTP && (m_pTFTPFileServer != 0)) {
		const uint32_t nFileSize = m_pTFTPFileServer->GetFileSize();
		DEBUG_PRINTF("nFileSize=%d", (int) nFileSize);

		bool bSucces = true;

		if (nFileSize != 0) {
#ifndef NDEBUG
			debug_dump((void *)m_pTFTPBuffer, 512);
#endif
			bSucces = SpiFlashInstall::Get()->WriteFirmware(m_pTFTPBuffer, nFileSize);

			if (!bSucces) {
				Display::Get()->Status(DISPLAY_7SEGMENT_MSG_ERROR_TFTP);
			}
		}

		DEBUG_PUTS("Delete TFTP Server");

		delete m_pTFTPFileServer;
		m_pTFTPFileServer = 0;

		delete[] m_pTFTPBuffer;
		m_pTFTPBuffer = 0;

		if (bSucces) { // Keep error message
			Display::Get()->Status(DISPLAY_7SEGMENT_MSG_INFO_TFTP_OFF);
		}
	}

	DEBUG_EXIT
}

void RemoteConfig::HandleTftpGet(void) {
	DEBUG_ENTRY

	if (m_nBytesReceived == GET_TFTP_LENGTH) {
		const uint32_t nLength = snprintf((char *)m_pUdpBuffer, UDP_BUFFER_SIZE, "tftp:%s\n", m_bEnableTFTP ? "On" : "Off");
		Network::Get()->SendTo(m_nHandle, (const uint8_t *)m_pUdpBuffer, nLength, m_nIPAddressFrom, (uint16_t) UDP_PORT);
	} else if (m_nBytesReceived == GET_TFTP_LENGTH + 3) {
		DEBUG_PUTS("Check for \'bin\' parameter");
		if (memcmp((const void *)&	m_pUdpBuffer[GET_TFTP_LENGTH], "bin", 3) == 0) {
			Network::Get()->SendTo(m_nHandle, (const uint8_t *)&m_bEnableTFTP, sizeof(bool) , m_nIPAddressFrom, (uint16_t) UDP_PORT);
		}
	}

	DEBUG_EXIT
}
