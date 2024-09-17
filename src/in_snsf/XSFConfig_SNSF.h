/*
 * xSF - SNSF configuration
 * By Naram Qashat (CyberBotX) [cyberbotx@cyberbotx.com]
 *
 * Partially based on the vio*sf framework
 *
 * NOTE: 16-bit sound is always enabled, the player is currently limited to
 * creating a 16-bit PCM for Winamp and can not handle 8-bit sound from
 * snes9x.
 */

#pragma once

#include <bitset>
#include <string>
#include "windowsh_wrapper.h"
#include "XSFConfig.h"

class XSFPlayer;

class XSFConfig_SNSF : public XSFConfig
{
protected:
	static bool /*initSixteenBitSound, */initReverseStereo;
	static unsigned initResampler;
	static std::string initMutes;

	friend class XSFConfig;
	bool /*sixteenBitSound, */reverseStereo;
	std::bitset<8> mutes;

	XSFConfig_SNSF();
	void LoadSpecificConfig() override;
	void SaveSpecificConfig() override;
	void GenerateSpecificDialogs() override;
	INT_PTR CALLBACK ConfigDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
	void ResetSpecificConfigDefaults(HWND hwndDlg) override;
	void SaveSpecificConfigDialog(HWND hwndDlg) override;
	void CopySpecificConfigToMemory(XSFPlayer *xSFPlayer, bool preLoad) override;
public:
	unsigned resampler;

	void About(HWND parent) override;
};
