/*
 * xSF - SNSF Player
 * By Naram Qashat (CyberBotX) [cyberbotx@cyberbotx.com]
 *
 * Based on a modified in_snsf by Caitsith2
 * http://snsf.caitsith2.net/
 *
 * Partially based on the vio*sf framework
 *
 * Utilizes a modified snes9x v1.53 for playback
 * http://www.snes9x.com/
 */

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <zlib.h>
#include "XSFCommon.h"
#include "XSFConfig_SNSF.h"
#include "XSFPlayer.h"

#undef min
#undef max

#include "snes9x/apu/apu.h"
#include "snes9x/apu/linear_resampler.h"
#include "snes9x/apu/hermite_resampler.h"
#include "snes9x/apu/bspline_resampler.h"
#include "snes9x/apu/osculating_resampler.h"
#include "snes9x/apu/sinc_resampler.h"
#include "snes9x/memmap.h"

class XSFPlayer_SNSF : public XSFPlayer
{
public:
	XSFPlayer_SNSF(const std::filesystem::path &path);
	~XSFPlayer_SNSF() { this->Terminate(); }
	bool Load();
	void GenerateSamples(std::vector<std::uint8_t> &buf, unsigned offset, unsigned samples);
	void Terminate();
};

const char *XSFPlayer::WinampDescription = "SNSF Decoder";
const char *XSFPlayer::WinampExts = "snsf;minisnsf\0SNES Sound Format files (*.snsf;*.minisnsf)\0";

extern XSFConfig *xSFConfig;

XSFPlayer *XSFPlayer::Create(const std::filesystem::path &path)
{
	return new XSFPlayer_SNSF(path);
}

static struct
{
	std::vector<std::uint8_t> rom, sram;
	bool first;
	unsigned base;
} loaderwork = { std::vector<std::uint8_t>(), std::vector<std::uint8_t>(), false, 0 };

class BUFFER
{
public:
	std::vector<std::uint8_t> buf;
	unsigned fil, cur, len;
	BUFFER() : buf(), fil(0), cur(0), len(0) { }
	bool Init()
	{
		if (!this->buf.empty())
			this->buf.clear();
		this->len = 2 * 2 * 48000 / 5;
		this->buf.resize(len, 0);
		this->fil = this->cur = 0;
		return true;
	}
	void Fill()
	{
		S9xSyncSound();
		S9xMainLoop();
		this->Mix();
	}
	void Mix()
	{
		unsigned bytes = (S9xGetSampleCount() << 1) & ~3;
		unsigned bleft = (this->len - this->fil) & ~3;
		if (!bytes)
			return;
		if (bytes > bleft)
			bytes = bleft;
		std::fill_n(&this->buf[this->fil], bytes, 0);
		S9xMixSamples(&this->buf[this->fil], bytes >> 1);
		this->fil += bytes;
	}
};
static BUFFER buffer;

bool S9xOpenSoundDevice()
{
	return true;
}

static void MapSNSFSection(const std::vector<std::uint8_t> &section)
{
	auto &data = loaderwork.rom;

	std::uint32_t offset = Get32BitsLE(&section[0]), size = Get32BitsLE(&section[4]), finalSize = size + offset;
	if (!loaderwork.first)
	{
		loaderwork.first = true;
		loaderwork.base = offset;
	}
	else
		offset += loaderwork.base;
	offset &= 0x1FFFFFFF;
	if (data.empty())
		data.resize(finalSize, 0);
	else if (data.size() < size + offset)
		data.resize(offset + finalSize);
	std::copy_n(&section[8], size, &data[offset]);
}

static bool MapSNSF(XSFFile *xSF)
{
	if (!xSF->IsValidType(0x23))
		return false;

	auto &reservedSection = xSF->GetReservedSection(), &programSection = xSF->GetProgramSection();

	if (!reservedSection.empty())
	{
		std::size_t reservedPosition = 0, reservedSize = reservedSection.size();
		while (reservedPosition + 8 < reservedSize)
		{
			std::uint32_t type = Get32BitsLE(&reservedSection[reservedPosition]), size = Get32BitsLE(&reservedSection[reservedPosition + 4]);
			if (!type)
			{
				if (loaderwork.sram.empty())
					loaderwork.sram.resize(0x20000, 0xFF);
				if (reservedPosition + 8 + size > reservedSize)
					return false;
				std::uint32_t offset = Get32BitsLE(&reservedSection[reservedPosition + 8]);
				if (size > 4 && loaderwork.sram.size() > offset)
				{
					auto len = std::min(size - 4, loaderwork.sram.size() - offset);
					std::copy_n(&reservedSection[reservedPosition + 12], len, &loaderwork.sram[offset]);
				}
			}
			reservedPosition += size + 8;
		}
	}

	if (!programSection.empty())
		MapSNSFSection(programSection);

	return true;
}

static bool RecursiveLoadSNSF(XSFFile *xSF, int level)
{
	if (level <= 10 && xSF->GetTagExists("_lib"))
	{
		auto libxSF = std::make_unique<XSFFile>(xSF->GetFilepath().parent_path() / xSF->GetTagValue("_lib"), 4, 8);
		if (!RecursiveLoadSNSF(libxSF.get(), level + 1))
			return false;
	}

	if (!MapSNSF(xSF))
		return false;

	unsigned n = 2;
	bool found;
	do
	{
		found = false;
		std::string libTag = "_lib" + std::to_string(n++);
		if (xSF->GetTagExists(libTag))
		{
			found = true;
			auto libxSF = std::make_unique<XSFFile>(xSF->GetFilepath().parent_path() / xSF->GetTagValue(libTag), 4, 8);
			if (!RecursiveLoadSNSF(libxSF.get(), level + 1))
				return false;
		}
	} while (found);

	return true;
}

static bool LoadSNSF(XSFFile *xSF)
{
	loaderwork.rom.clear();
	loaderwork.sram.clear();
	loaderwork.first = false;
	loaderwork.base = 0;

	return RecursiveLoadSNSF(xSF, 1);
}

XSFPlayer_SNSF::XSFPlayer_SNSF(const std::filesystem::path &path) : XSFPlayer()
{
	this->xSF.reset(new XSFFile(path, 4, 8));
}

bool XSFPlayer_SNSF::Load()
{
	if (!LoadSNSF(this->xSF.get()))
		return false;

	Settings.SoundSync = true;
	Settings.Mute = false;
	Settings.SoundPlaybackRate = this->sampleRate;
	Settings.SixteenBitSound = true;
	Settings.Stereo = true;

	Memory.Init();

	S9xInitAPU();
	XSFConfig_SNSF *xSFConfig_SNSF = dynamic_cast<XSFConfig_SNSF *>(xSFConfig);
	if (xSFConfig_SNSF->resampler == 4)
		S9xInitSound<SincResampler>(10, 0);
	else if (xSFConfig_SNSF->resampler == 3)
		S9xInitSound<OsculatingResampler>(10, 0);
	else if (xSFConfig_SNSF->resampler == 2)
		S9xInitSound<BsplineResampler>(10, 0);
	else if (xSFConfig_SNSF->resampler == 1)
		S9xInitSound<HermiteResampler>(10, 0);
	else
		S9xInitSound<LinearResampler>(10, 0);

	if (!buffer.Init())
		return false;

	if (!Memory.LoadROMSNSF(&loaderwork.rom[0], loaderwork.rom.size(), &loaderwork.sram[0], loaderwork.sram.size()))
		return false;

	//S9xSetPlaybackRate(Settings.SoundPlaybackRate);
	S9xSetSoundMute(false);

	// bad hack for gradius3snsf.rar
	//Settings.TurboMode = true;

	return XSFPlayer::Load();
}

void XSFPlayer_SNSF::GenerateSamples(std::vector<std::uint8_t> &buf, unsigned offset, unsigned samples)
{
	unsigned bytes = samples << 2;
	while (bytes)
	{
		unsigned remain = buffer.fil - buffer.cur;
		while (!remain)
		{
			buffer.cur = buffer.fil = 0;
			buffer.Fill();

			remain = buffer.fil - buffer.cur;
		}
		unsigned len = remain;
		if (len > bytes)
			len = bytes;
		std::copy_n(&buffer.buf[buffer.cur], len, &buf[offset]);
		bytes -= len;
		offset += len;
		buffer.cur += len;
	}
}

void XSFPlayer_SNSF::Terminate()
{
	S9xReset();
	Memory.Deinit();
	S9xDeinitAPU();

	loaderwork.rom.clear();
	loaderwork.sram.clear();
	loaderwork.first = false;
	loaderwork.base = 0;
}
