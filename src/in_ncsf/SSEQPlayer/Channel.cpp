/*
 * SSEQ Player - Channel structures
 * By Naram Qashat (CyberBotX) [cyberbotx@cyberbotx.com]
 * Last modification on 2014-10-27
 *
 * Adapted from source code of FeOS Sound System
 * By fincs
 * https://github.com/fincs/FSS
 *
 * Some code/concepts from DeSmuME
 * http://desmume.org/
 */

#include "XSFCommon.h"
#include "Channel.h"
#include "Player.h"
#include "common.h"

NDSSoundRegister::NDSSoundRegister() : volumeMul(0), volumeDiv(0), panning(0), waveDuty(0), repeatMode(0), format(0), enable(false),
	source(nullptr), timer(0), psgX(0), psgLast(0), psgLastCount(0), samplePosition(0), sampleIncrease(0), loopStart(0), length(0), totalLength(0)
{
}

void NDSSoundRegister::ClearControlRegister()
{
	this->volumeMul = this->volumeDiv = this->panning = this->waveDuty = this->repeatMode = this->format = 0;
	this->enable = false;
}

void NDSSoundRegister::SetControlRegister(uint32_t reg)
{
	this->volumeMul = reg & 0x7F;
	this->volumeDiv = (reg >> 8) & 0x03;
	this->panning = (reg >> 16) & 0x7F;
	this->waveDuty = (reg >> 24) & 0x07;
	this->repeatMode = (reg >> 27) & 0x03;
	this->format = (reg >> 29) & 0x03;
	this->enable = (reg >> 31) & 0x01;
}

TempSndReg::TempSndReg() : CR(0), SOURCE(nullptr), TIMER(0), REPEAT_POINT(0), LENGTH(0)
{
}

bool Channel::initializedLUTs = false;
double Channel::sinc_lut[Channel::SINC_SAMPLES + 1];
double Channel::window_lut[Channel::SINC_SAMPLES + 1];

#ifndef M_PI
static const double M_PI = 3.14159265358979323846;
#endif

static inline double sinc(double x)
{
	return fEqual(x, 0.0) ? 1.0 : std::sin(x * M_PI) / (x * M_PI);
}

Channel::Channel() : chnId(-1), tempReg(), state(CS_NONE), trackId(-1), prio(0), manualSweep(false), flags(), pan(0), extAmpl(0), velocity(0), extPan(0),
	key(0), ampl(0), extTune(0), orgKey(0), modType(0), modSpeed(0), modDepth(0), modRange(0), modDelay(0), modDelayCnt(0), modCounter(0),
	sweepLen(0), sweepCnt(0), sweepPitch(0), attackLvl(0), sustainLvl(0x7F), decayRate(0), releaseRate(0xFFFF), noteLength(-1), vol(0), ply(nullptr), reg(),
	ringBuffer()
{
	if (!this->initializedLUTs)
	{
		double dx = static_cast<double>(SINC_WIDTH) / SINC_SAMPLES, x = 0.0;
		for (unsigned i = 0; i <= SINC_SAMPLES; ++i, x += dx)
		{
			double y = x / SINC_WIDTH;
			this->sinc_lut[i] = std::abs(x) < SINC_WIDTH ? sinc(x) : 0.0;
			this->window_lut[i] = 0.40897 + 0.5 * std::cos(M_PI * y) + 0.09103 * std::cos(2 * M_PI * y);
		}
		this->initializedLUTs = true;
	}
}

// Original FSS Function: Chn_UpdateVol
void Channel::UpdateVol(const Track &trk)
{
	int finalVol = trk.ply->masterVol;
	finalVol += trk.ply->sseqVol;
	finalVol += Cnv_Sust(trk.vol);
	finalVol += Cnv_Sust(trk.expr);
	if (finalVol < -AMPL_K)
		finalVol = -AMPL_K;
	this->extAmpl = finalVol;
}

// Original FSS Function: Chn_UpdatePan
void Channel::UpdatePan(const Track &trk)
{
	this->extPan = trk.pan;
}

// Original FSS Function: Chn_UpdateTune
void Channel::UpdateTune(const Track &trk)
{
	int tune = (static_cast<int>(this->key) - static_cast<int>(this->orgKey)) * 64;
	tune += (static_cast<int>(trk.pitchBend) * static_cast<int>(trk.pitchBendRange)) >> 1;
	this->extTune = tune;
}

// Original FSS Function: Chn_UpdateMod
void Channel::UpdateMod(const Track &trk)
{
	this->modType = trk.modType;
	this->modSpeed = trk.modSpeed;
	this->modDepth = trk.modDepth;
	this->modRange = trk.modRange;
	this->modDelay = trk.modDelay;
}

// Original FSS Function: Chn_UpdatePorta
void Channel::UpdatePorta(const Track &trk)
{
	this->manualSweep = false;
	this->sweepPitch = trk.sweepPitch;
	this->sweepCnt = 0;
	if (!trk.state[TS_PORTABIT])
	{
		this->sweepLen = 0;
		return;
	}

	int diff = (static_cast<int>(trk.portaKey) - static_cast<int>(this->key)) << 22;
	this->sweepPitch += diff >> 16;

	if (!trk.portaTime)
	{
		this->sweepLen = this->noteLength;
		this->manualSweep = true;
	}
	else
	{
		int sq_time = static_cast<uint32_t>(trk.portaTime) * static_cast<uint32_t>(trk.portaTime);
		int abs_sp = std::abs(this->sweepPitch);
		this->sweepLen = (abs_sp * sq_time) >> 11;
	}
}

// Original FSS Function: Chn_Release
void Channel::Release()
{
	this->noteLength = -1;
	this->prio = 1;
	this->state = CS_RELEASE;
}

// Original FSS Function: Chn_Kill
void Channel::Kill()
{
	this->state = CS_NONE;
	this->trackId = -1;
	this->prio = 0;
	this->reg.ClearControlRegister();
	this->vol = 0;
	this->noteLength = -1;
}

static inline int getModFlag(int type)
{
	switch (type)
	{
		case 0:
			return CF_UPDTMR;
		case 1:
			return CF_UPDVOL;
		case 2:
			return CF_UPDPAN;
		default:
			return 0;
	}
}

// Original FSS Function: Chn_UpdateTracks
void Channel::UpdateTrack()
{
	if (!this->ply)
		return;

	int trkn = this->trackId;
	if (trkn == -1)
		return;

	auto &trk = this->ply->tracks[trkn];
	auto &trackFlags = trk.updateFlags;
	if (trackFlags.none())
		return;

	if (trackFlags[TUF_LEN])
	{
		int st = this->state;
		if (st > CS_START)
		{
			if (st < CS_RELEASE && !--this->noteLength)
				this->Release();
			if (this->manualSweep && this->sweepCnt < this->sweepLen)
				++this->sweepCnt;
		}
	}
	if (trackFlags[TUF_VOL])
	{
		this->UpdateVol(trk);
		this->flags.set(CF_UPDVOL);
	}
	if (trackFlags[TUF_PAN])
	{
		this->UpdatePan(trk);
		this->flags.set(CF_UPDPAN);
	}
	if (trackFlags[TUF_TIMER])
	{
		this->UpdateTune(trk);
		this->flags.set(CF_UPDTMR);
	}
	if (trackFlags[TUF_MOD])
	{
		int oldType = this->modType;
		int newType = trk.modType;
		this->UpdateMod(trk);
		if (oldType != newType)
		{
			this->flags.set(getModFlag(oldType));
			this->flags.set(getModFlag(newType));
		}
	}
}

static const uint16_t getpitchtbl[] =
{
	0x0000, 0x003B, 0x0076, 0x00B2, 0x00ED, 0x0128, 0x0164, 0x019F,
	0x01DB, 0x0217, 0x0252, 0x028E, 0x02CA, 0x0305, 0x0341, 0x037D,
	0x03B9, 0x03F5, 0x0431, 0x046E, 0x04AA, 0x04E6, 0x0522, 0x055F,
	0x059B, 0x05D8, 0x0614, 0x0651, 0x068D, 0x06CA, 0x0707, 0x0743,
	0x0780, 0x07BD, 0x07FA, 0x0837, 0x0874, 0x08B1, 0x08EF, 0x092C,
	0x0969, 0x09A7, 0x09E4, 0x0A21, 0x0A5F, 0x0A9C, 0x0ADA, 0x0B18,
	0x0B56, 0x0B93, 0x0BD1, 0x0C0F, 0x0C4D, 0x0C8B, 0x0CC9, 0x0D07,
	0x0D45, 0x0D84, 0x0DC2, 0x0E00, 0x0E3F, 0x0E7D, 0x0EBC, 0x0EFA,
	0x0F39, 0x0F78, 0x0FB6, 0x0FF5, 0x1034, 0x1073, 0x10B2, 0x10F1,
	0x1130, 0x116F, 0x11AE, 0x11EE, 0x122D, 0x126C, 0x12AC, 0x12EB,
	0x132B, 0x136B, 0x13AA, 0x13EA, 0x142A, 0x146A, 0x14A9, 0x14E9,
	0x1529, 0x1569, 0x15AA, 0x15EA, 0x162A, 0x166A, 0x16AB, 0x16EB,
	0x172C, 0x176C, 0x17AD, 0x17ED, 0x182E, 0x186F, 0x18B0, 0x18F0,
	0x1931, 0x1972, 0x19B3, 0x19F5, 0x1A36, 0x1A77, 0x1AB8, 0x1AFA,
	0x1B3B, 0x1B7D, 0x1BBE, 0x1C00, 0x1C41, 0x1C83, 0x1CC5, 0x1D07,
	0x1D48, 0x1D8A, 0x1DCC, 0x1E0E, 0x1E51, 0x1E93, 0x1ED5, 0x1F17,
	0x1F5A, 0x1F9C, 0x1FDF, 0x2021, 0x2064, 0x20A6, 0x20E9, 0x212C,
	0x216F, 0x21B2, 0x21F5, 0x2238, 0x227B, 0x22BE, 0x2301, 0x2344,
	0x2388, 0x23CB, 0x240E, 0x2452, 0x2496, 0x24D9, 0x251D, 0x2561,
	0x25A4, 0x25E8, 0x262C, 0x2670, 0x26B4, 0x26F8, 0x273D, 0x2781,
	0x27C5, 0x280A, 0x284E, 0x2892, 0x28D7, 0x291C, 0x2960, 0x29A5,
	0x29EA, 0x2A2F, 0x2A74, 0x2AB9, 0x2AFE, 0x2B43, 0x2B88, 0x2BCD,
	0x2C13, 0x2C58, 0x2C9D, 0x2CE3, 0x2D28, 0x2D6E, 0x2DB4, 0x2DF9,
	0x2E3F, 0x2E85, 0x2ECB, 0x2F11, 0x2F57, 0x2F9D, 0x2FE3, 0x302A,
	0x3070, 0x30B6, 0x30FD, 0x3143, 0x318A, 0x31D0, 0x3217, 0x325E,
	0x32A5, 0x32EC, 0x3332, 0x3379, 0x33C1, 0x3408, 0x344F, 0x3496,
	0x34DD, 0x3525, 0x356C, 0x35B4, 0x35FB, 0x3643, 0x368B, 0x36D3,
	0x371A, 0x3762, 0x37AA, 0x37F2, 0x383A, 0x3883, 0x38CB, 0x3913,
	0x395C, 0x39A4, 0x39ED, 0x3A35, 0x3A7E, 0x3AC6, 0x3B0F, 0x3B58,
	0x3BA1, 0x3BEA, 0x3C33, 0x3C7C, 0x3CC5, 0x3D0E, 0x3D58, 0x3DA1,
	0x3DEA, 0x3E34, 0x3E7D, 0x3EC7, 0x3F11, 0x3F5A, 0x3FA4, 0x3FEE,
	0x4038, 0x4082, 0x40CC, 0x4116, 0x4161, 0x41AB, 0x41F5, 0x4240,
	0x428A, 0x42D5, 0x431F, 0x436A, 0x43B5, 0x4400, 0x444B, 0x4495,
	0x44E1, 0x452C, 0x4577, 0x45C2, 0x460D, 0x4659, 0x46A4, 0x46F0,
	0x473B, 0x4787, 0x47D3, 0x481E, 0x486A, 0x48B6, 0x4902, 0x494E,
	0x499A, 0x49E6, 0x4A33, 0x4A7F, 0x4ACB, 0x4B18, 0x4B64, 0x4BB1,
	0x4BFE, 0x4C4A, 0x4C97, 0x4CE4, 0x4D31, 0x4D7E, 0x4DCB, 0x4E18,
	0x4E66, 0x4EB3, 0x4F00, 0x4F4E, 0x4F9B, 0x4FE9, 0x5036, 0x5084,
	0x50D2, 0x5120, 0x516E, 0x51BC, 0x520A, 0x5258, 0x52A6, 0x52F4,
	0x5343, 0x5391, 0x53E0, 0x542E, 0x547D, 0x54CC, 0x551A, 0x5569,
	0x55B8, 0x5607, 0x5656, 0x56A5, 0x56F4, 0x5744, 0x5793, 0x57E2,
	0x5832, 0x5882, 0x58D1, 0x5921, 0x5971, 0x59C1, 0x5A10, 0x5A60,
	0x5AB0, 0x5B01, 0x5B51, 0x5BA1, 0x5BF1, 0x5C42, 0x5C92, 0x5CE3,
	0x5D34, 0x5D84, 0x5DD5, 0x5E26, 0x5E77, 0x5EC8, 0x5F19, 0x5F6A,
	0x5FBB, 0x600D, 0x605E, 0x60B0, 0x6101, 0x6153, 0x61A4, 0x61F6,
	0x6248, 0x629A, 0x62EC, 0x633E, 0x6390, 0x63E2, 0x6434, 0x6487,
	0x64D9, 0x652C, 0x657E, 0x65D1, 0x6624, 0x6676, 0x66C9, 0x671C,
	0x676F, 0x67C2, 0x6815, 0x6869, 0x68BC, 0x690F, 0x6963, 0x69B6,
	0x6A0A, 0x6A5E, 0x6AB1, 0x6B05, 0x6B59, 0x6BAD, 0x6C01, 0x6C55,
	0x6CAA, 0x6CFE, 0x6D52, 0x6DA7, 0x6DFB, 0x6E50, 0x6EA4, 0x6EF9,
	0x6F4E, 0x6FA3, 0x6FF8, 0x704D, 0x70A2, 0x70F7, 0x714D, 0x71A2,
	0x71F7, 0x724D, 0x72A2, 0x72F8, 0x734E, 0x73A4, 0x73FA, 0x7450,
	0x74A6, 0x74FC, 0x7552, 0x75A8, 0x75FF, 0x7655, 0x76AC, 0x7702,
	0x7759, 0x77B0, 0x7807, 0x785E, 0x78B4, 0x790C, 0x7963, 0x79BA,
	0x7A11, 0x7A69, 0x7AC0, 0x7B18, 0x7B6F, 0x7BC7, 0x7C1F, 0x7C77,
	0x7CCF, 0x7D27, 0x7D7F, 0x7DD7, 0x7E2F, 0x7E88, 0x7EE0, 0x7F38,
	0x7F91, 0x7FEA, 0x8042, 0x809B, 0x80F4, 0x814D, 0x81A6, 0x81FF,
	0x8259, 0x82B2, 0x830B, 0x8365, 0x83BE, 0x8418, 0x8472, 0x84CB,
	0x8525, 0x857F, 0x85D9, 0x8633, 0x868E, 0x86E8, 0x8742, 0x879D,
	0x87F7, 0x8852, 0x88AC, 0x8907, 0x8962, 0x89BD, 0x8A18, 0x8A73,
	0x8ACE, 0x8B2A, 0x8B85, 0x8BE0, 0x8C3C, 0x8C97, 0x8CF3, 0x8D4F,
	0x8DAB, 0x8E07, 0x8E63, 0x8EBF, 0x8F1B, 0x8F77, 0x8FD4, 0x9030,
	0x908C, 0x90E9, 0x9146, 0x91A2, 0x91FF, 0x925C, 0x92B9, 0x9316,
	0x9373, 0x93D1, 0x942E, 0x948C, 0x94E9, 0x9547, 0x95A4, 0x9602,
	0x9660, 0x96BE, 0x971C, 0x977A, 0x97D8, 0x9836, 0x9895, 0x98F3,
	0x9952, 0x99B0, 0x9A0F, 0x9A6E, 0x9ACD, 0x9B2C, 0x9B8B, 0x9BEA,
	0x9C49, 0x9CA8, 0x9D08, 0x9D67, 0x9DC7, 0x9E26, 0x9E86, 0x9EE6,
	0x9F46, 0x9FA6, 0xA006, 0xA066, 0xA0C6, 0xA127, 0xA187, 0xA1E8,
	0xA248, 0xA2A9, 0xA30A, 0xA36B, 0xA3CC, 0xA42D, 0xA48E, 0xA4EF,
	0xA550, 0xA5B2, 0xA613, 0xA675, 0xA6D6, 0xA738, 0xA79A, 0xA7FC,
	0xA85E, 0xA8C0, 0xA922, 0xA984, 0xA9E7, 0xAA49, 0xAAAC, 0xAB0E,
	0xAB71, 0xABD4, 0xAC37, 0xAC9A, 0xACFD, 0xAD60, 0xADC3, 0xAE27,
	0xAE8A, 0xAEED, 0xAF51, 0xAFB5, 0xB019, 0xB07C, 0xB0E0, 0xB145,
	0xB1A9, 0xB20D, 0xB271, 0xB2D6, 0xB33A, 0xB39F, 0xB403, 0xB468,
	0xB4CD, 0xB532, 0xB597, 0xB5FC, 0xB662, 0xB6C7, 0xB72C, 0xB792,
	0xB7F7, 0xB85D, 0xB8C3, 0xB929, 0xB98F, 0xB9F5, 0xBA5B, 0xBAC1,
	0xBB28, 0xBB8E, 0xBBF5, 0xBC5B, 0xBCC2, 0xBD29, 0xBD90, 0xBDF7,
	0xBE5E, 0xBEC5, 0xBF2C, 0xBF94, 0xBFFB, 0xC063, 0xC0CA, 0xC132,
	0xC19A, 0xC202, 0xC26A, 0xC2D2, 0xC33A, 0xC3A2, 0xC40B, 0xC473,
	0xC4DC, 0xC544, 0xC5AD, 0xC616, 0xC67F, 0xC6E8, 0xC751, 0xC7BB,
	0xC824, 0xC88D, 0xC8F7, 0xC960, 0xC9CA, 0xCA34, 0xCA9E, 0xCB08,
	0xCB72, 0xCBDC, 0xCC47, 0xCCB1, 0xCD1B, 0xCD86, 0xCDF1, 0xCE5B,
	0xCEC6, 0xCF31, 0xCF9C, 0xD008, 0xD073, 0xD0DE, 0xD14A, 0xD1B5,
	0xD221, 0xD28D, 0xD2F8, 0xD364, 0xD3D0, 0xD43D, 0xD4A9, 0xD515,
	0xD582, 0xD5EE, 0xD65B, 0xD6C7, 0xD734, 0xD7A1, 0xD80E, 0xD87B,
	0xD8E9, 0xD956, 0xD9C3, 0xDA31, 0xDA9E, 0xDB0C, 0xDB7A, 0xDBE8,
	0xDC56, 0xDCC4, 0xDD32, 0xDDA0, 0xDE0F, 0xDE7D, 0xDEEC, 0xDF5B,
	0xDFC9, 0xE038, 0xE0A7, 0xE116, 0xE186, 0xE1F5, 0xE264, 0xE2D4,
	0xE343, 0xE3B3, 0xE423, 0xE493, 0xE503, 0xE573, 0xE5E3, 0xE654,
	0xE6C4, 0xE735, 0xE7A5, 0xE816, 0xE887, 0xE8F8, 0xE969, 0xE9DA,
	0xEA4B, 0xEABC, 0xEB2E, 0xEB9F, 0xEC11, 0xEC83, 0xECF5, 0xED66,
	0xEDD9, 0xEE4B, 0xEEBD, 0xEF2F, 0xEFA2, 0xF014, 0xF087, 0xF0FA,
	0xF16D, 0xF1E0, 0xF253, 0xF2C6, 0xF339, 0xF3AD, 0xF420, 0xF494,
	0xF507, 0xF57B, 0xF5EF, 0xF663, 0xF6D7, 0xF74C, 0xF7C0, 0xF834,
	0xF8A9, 0xF91E, 0xF992, 0xFA07, 0xFA7C, 0xFAF1, 0xFB66, 0xFBDC,
	0xFC51, 0xFCC7, 0xFD3C, 0xFDB2, 0xFE28, 0xFE9E, 0xFF14, 0xFF8A
};

static const uint8_t getvoltbl[] =
{
	0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
	0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
	0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
	0x05, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
	0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x08, 0x08, 0x08, 0x08,
	0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
	0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
	0x0B, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E,
	0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13, 0x14,
	0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x18,
	0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19, 0x1A, 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1C, 0x1C, 0x1C,
	0x1D, 0x1D, 0x1D, 0x1E, 0x1E, 0x1E, 0x1F, 0x1F, 0x1F, 0x20, 0x20, 0x20, 0x21, 0x21, 0x22, 0x22,
	0x22, 0x23, 0x23, 0x24, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27, 0x27, 0x28, 0x28, 0x29,
	0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F, 0x30, 0x31, 0x31,
	0x32, 0x32, 0x33, 0x33, 0x34, 0x35, 0x35, 0x36, 0x36, 0x37, 0x38, 0x38, 0x39, 0x3A, 0x3A, 0x3B,
	0x3C, 0x3C, 0x3D, 0x3E, 0x3F, 0x3F, 0x40, 0x41, 0x42, 0x42, 0x43, 0x44, 0x45, 0x45, 0x46, 0x47,
	0x48, 0x49, 0x4A, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x52, 0x53, 0x54, 0x55,
	0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x67,
	0x68, 0x69, 0x6A, 0x6B, 0x6D, 0x6E, 0x6F, 0x71, 0x72, 0x73, 0x75, 0x76, 0x77, 0x79, 0x7A, 0x7B,
	0x7D, 0x7E, 0x7F, 0x20, 0x21, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25,
	0x26, 0x26, 0x26, 0x27, 0x27, 0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D,
	0x2D, 0x2E, 0x2E, 0x2F, 0x2F, 0x30, 0x30, 0x31, 0x31, 0x32, 0x33, 0x33, 0x34, 0x34, 0x35, 0x36,
	0x36, 0x37, 0x37, 0x38, 0x39, 0x39, 0x3A, 0x3B, 0x3B, 0x3C, 0x3D, 0x3E, 0x3E, 0x3F, 0x40, 0x40,
	0x41, 0x42, 0x43, 0x43, 0x44, 0x45, 0x46, 0x47, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4D,
	0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D,
	0x5E, 0x5F, 0x60, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6F, 0x70,
	0x71, 0x73, 0x74, 0x75, 0x77, 0x78, 0x79, 0x7B, 0x7C, 0x7E, 0x7E, 0x40, 0x41, 0x42, 0x43, 0x43,
	0x44, 0x45, 0x46, 0x47, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51,
	0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61,
	0x62, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6B, 0x6C, 0x6D, 0x6E, 0x70, 0x71, 0x72, 0x74, 0x75,
	0x76, 0x78, 0x79, 0x7B, 0x7C, 0x7D, 0x7E, 0x40, 0x41, 0x42, 0x42, 0x43, 0x44, 0x45, 0x46, 0x46,
	0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
	0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x65, 0x66,
	0x67, 0x68, 0x69, 0x6A, 0x6C, 0x6D, 0x6E, 0x6F, 0x71, 0x72, 0x73, 0x75, 0x76, 0x77, 0x79, 0x7A,
	0x7C, 0x7D, 0x7E, 0x7F
};

// This function was obtained through disassembly of Ninty's sound driver
static inline uint16_t Timer_Adjust(uint16_t basetmr, int pitch)
{
	int shift = 0;
	pitch = -pitch;

	while (pitch < 0)
	{
		--shift;
		pitch += 0x300;
	}

	while (pitch >= 0x300)
	{
		++shift;
		pitch -= 0x300;
	}

	uint64_t tmr = static_cast<uint64_t>(basetmr) * (static_cast<uint32_t>(getpitchtbl[pitch]) + 0x10000);
	shift -= 16;
	if (shift <= 0)
		tmr >>= -shift;
	else if (shift < 32)
	{
		if (tmr & ((~0ULL) << (32 - shift)))
			return 0xFFFF;
		tmr <<= shift;
	}
	else
		return 0xFFFF;

	if (tmr < 0x10)
		return 0x10;
	if (tmr > 0xFFFF)
		return 0xFFFF;
	return static_cast<uint16_t>(tmr);
}

static inline int calcVolDivShift(int x)
{
	// VOLDIV(0) /1  >>0
	// VOLDIV(1) /2  >>1
	// VOLDIV(2) /4  >>2
	// VOLDIV(3) /16 >>4
	if (x < 3)
		return x;
	return 4;
}

// Original FSS Function: Snd_UpdChannel
void Channel::Update()
{
	// Kill active channels that aren't physically active
	if (this->state > CS_START && !this->reg.enable)
	{
		this->Kill();
		return;
	}

	bool bNotInSustain = this->state != CS_SUSTAIN;
	bool bInStart = this->state == CS_START;
	bool bPitchSweep = this->sweepPitch && this->sweepLen && this->sweepCnt <= this->sweepLen;
	bool bModulation = !!this->modDepth;
	bool bVolNeedUpdate = this->flags[CF_UPDVOL] || bNotInSustain;
	bool bPanNeedUpdate = this->flags[CF_UPDPAN] || bInStart;
	bool bTmrNeedUpdate = this->flags[CF_UPDTMR] || bInStart || bPitchSweep;
	int modParam = 0;

	switch (this->state)
	{
		case CS_NONE:
			return;
		case CS_START:
			this->reg.ClearControlRegister();
			this->reg.source = this->tempReg.SOURCE;
			this->reg.loopStart = this->tempReg.REPEAT_POINT;
			this->reg.length = this->tempReg.LENGTH;
			this->reg.totalLength = this->reg.loopStart + this->reg.length;
			this->ampl = AMPL_THRESHOLD;
			this->state = CS_ATTACK;
			// fallthrough
		case CS_ATTACK:
		{
			int newAmpl = this->ampl;
			int oldAmpl = this->ampl >> 7;
			do
				newAmpl = (newAmpl * static_cast<int>(this->attackLvl)) / 256;
			while ((newAmpl >> 7) == oldAmpl);
			this->ampl = newAmpl;
			if (!this->ampl)
				this->state = CS_DECAY;
			break;
		}
		case CS_DECAY:
		{
			this->ampl -= static_cast<int>(this->decayRate);
			int sustLvl = Cnv_Sust(this->sustainLvl) << 7;
			if (this->ampl <= sustLvl)
			{
				this->ampl = sustLvl;
				this->state = CS_SUSTAIN;
			}
			break;
		}
		case CS_RELEASE:
			this->ampl -= static_cast<int>(this->releaseRate);
			if (this->ampl <= AMPL_THRESHOLD)
			{
				this->Kill();
				return;
			}
	}

	if (bModulation && this->modDelayCnt < this->modDelay)
	{
		++this->modDelayCnt;
		bModulation = false;
	}

	if (bModulation)
	{
		switch (this->modType)
		{
			case 0:
				bTmrNeedUpdate = true;
				break;
			case 1:
				bVolNeedUpdate = true;
				break;
			case 2:
				bPanNeedUpdate = true;
		}

		// Get the current modulation parameter
		modParam = Cnv_Sine(this->modCounter >> 8) * this->modRange * this->modDepth; // 7.14

		if (this->modType == 1)
			modParam = static_cast<int64_t>(modParam * 60) >> 14; // vol: adjust range to 6dB = 60cB (no fractional bits)
		else
			modParam >>= 8; // tmr/pan: adjust to 7.6

		// Update the modulation variables
		uint32_t counter = this->modCounter + (this->modSpeed << 6);
		while (counter >= 0x8000)
			counter -= 0x8000;
		this->modCounter = counter;
	}

	if (bTmrNeedUpdate)
	{
		int totalAdj = this->extTune;
		if (bModulation && !this->modType)
			totalAdj += modParam;
		if (bPitchSweep)
		{
			int len = this->sweepLen;
			int cnt = this->sweepCnt;
			totalAdj += (static_cast<int64_t>(this->sweepPitch) * (len - cnt)) / len;
			if (!this->manualSweep)
				++this->sweepCnt;
		}
		uint16_t tmr = this->tempReg.TIMER;

		if (totalAdj)
			tmr = Timer_Adjust(tmr, totalAdj);
		this->reg.timer = -tmr;
		this->reg.sampleIncrease = (ARM7_CLOCK / static_cast<double>(this->ply->sampleRate * 2)) / (0x10000 - this->reg.timer);
		this->flags.reset(CF_UPDTMR);
	}

	if (bVolNeedUpdate || bPanNeedUpdate)
	{
		uint32_t cr = this->tempReg.CR;
		if (bVolNeedUpdate)
		{
			int totalVol = this->ampl >> 7;
			totalVol += this->extAmpl;
			totalVol += this->velocity;
			if (bModulation && this->modType == 1)
				totalVol += modParam;
			totalVol += AMPL_K;
			clamp(totalVol, 0, AMPL_K);

			cr &= ~(SOUND_VOL(0x7F) | SOUND_VOLDIV(3));
			cr |= SOUND_VOL(static_cast<int>(getvoltbl[totalVol]));

			if (totalVol < AMPL_K - 240)
				cr |= SOUND_VOLDIV(3);
			else if (totalVol < AMPL_K - 120)
				cr |= SOUND_VOLDIV(2);
			else if (totalVol < AMPL_K - 60)
				cr |= SOUND_VOLDIV(1);

			this->vol = ((cr & SOUND_VOL(0x7F)) << 4) >> calcVolDivShift((cr & SOUND_VOLDIV(3)) >> 8);

			this->flags.reset(CF_UPDVOL);
		}

		if (bPanNeedUpdate)
		{
			int realPan = this->pan;
			realPan += this->extPan;
			if (bModulation && this->modType == 2)
				realPan += modParam;
			realPan += 64;
			clamp(realPan, 0, 127);

			cr &= ~SOUND_PAN(0x7F);
			cr |= SOUND_PAN(realPan);
			this->flags.reset(CF_UPDPAN);
		}

		this->tempReg.CR = cr;
		this->reg.SetControlRegister(cr);
	}
}

static const int16_t wavedutytbl[8][8] =
{
	{ -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0x7FFF },
	{ -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0x7FFF, 0x7FFF },
	{ -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF },
	{ -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF },
	{ -0x7FFF, -0x7FFF, -0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF },
	{ -0x7FFF, -0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF },
	{ -0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF },
	{ -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF }
};

// Linear interpolation code originally from DeSmuME
// Legrange comes from Olli Niemitalo:
// http://www.student.oulu.fi/~oniemita/dsp/deip.pdf
int32_t Channel::Interpolate()
{
	double ratio = this->reg.samplePosition;
	ratio -= static_cast<int32_t>(ratio);

	const auto &data = this->ringBuffer.GetBuffer();

	if (this->ply->interpolation == INTERPOLATION_SINC)
	{
		double kernel[SINC_WIDTH * 2], kernel_sum = 0.0;
		int i = SINC_WIDTH, shift = static_cast<int>(std::floor(ratio * SINC_RESOLUTION));
		int step = this->reg.sampleIncrease > 1.0 ? static_cast<int>(SINC_RESOLUTION / this->reg.sampleIncrease) : SINC_RESOLUTION;
		int shift_adj = shift * step / SINC_RESOLUTION;
		int window_step = SINC_RESOLUTION;
		for (; i >= -static_cast<int>(SINC_WIDTH - 1); --i)
		{
			int pos = i * step;
			int window_pos = i * window_step;
			kernel_sum += kernel[i + SINC_WIDTH - 1] = this->sinc_lut[std::abs(shift_adj - pos)] * this->window_lut[std::abs(shift - window_pos)];
		}
		double sum = 0.0;
		for (i = 0; i < static_cast<int>(SINC_WIDTH * 2); ++i)
			sum += data[i - static_cast<int>(SINC_WIDTH) + 1] * kernel[i];
		return static_cast<int32_t>(sum / kernel_sum);
	}
	else if (this->ply->interpolation > INTERPOLATION_LINEAR)
	{
		double c0, c1, c2, c3, c4, c5;

		if (this->ply->interpolation == INTERPOLATION_6POINTLEGRANGE)
		{
			ratio -= 0.5;
			double even1 = data[-2] + data[3], odd1 = data[-2] - data[3];
			double even2 = data[-1] + data[2], odd2 = data[-1] - data[2];
			double even3 = data[0] + data[1], odd3 = data[0] - data[1];
			c0 = 0.01171875 * even1 - 0.09765625 * even2 + 0.5859375 * even3;
			c1 = 25 / 384.0 * odd2 - 1.171875 * odd3 - 0.0046875 * odd1;
			c2 = 0.40625 * even2 - 17 / 48.0 * even3 - 5 / 96.0 * even1;
			c3 = 1 / 48.0 * odd1 - 13 / 48.0 * odd2 + 17 / 24.0 * odd3;
			c4 = 1 / 48.0 * even1 - 0.0625 * even2 + 1 / 24.0 * even3;
			c5 = 1 / 24.0 * odd2 - 1 / 12.0 * odd3 - 1 / 120.0 * odd1;
			return static_cast<int32_t>(((((c5 * ratio + c4) * ratio + c3) * ratio + c2) * ratio + c1) * ratio + c0);
		}
		else // INTERPOLATION_4POINTLEAGRANGE
		{
			c0 = data[0];
			c1 = data[1] - 1 / 3.0 * data[-1] - 0.5 * data[0] - 1 / 6.0 * data[2];
			c2 = 0.5 * (data[-1] + data[1]) - data[0];
			c3 = 1 / 6.0 * (data[2] - data[-1]) + 0.5 * (data[0] - data[1]);
			return static_cast<int32_t>(((c3 * ratio + c2) * ratio + c1) * ratio + c0);
		}
	}
	else // INTERPOLATION_LINEAR
		return static_cast<int32_t>(data[0] + ratio * (data[1] - data[0]));
}

int32_t Channel::GenerateSample()
{
	if (this->reg.samplePosition < 0)
		return 0;

	if (this->reg.format != 3)
	{
		if (this->ply->interpolation == INTERPOLATION_NONE)
			return this->reg.source->dataptr[static_cast<uint32_t>(this->reg.samplePosition)];
		else
			return this->Interpolate();
	}
	else
	{
		if (this->chnId < 8)
			return 0;
		else if (this->chnId < 14)
			return wavedutytbl[this->reg.waveDuty][static_cast<uint32_t>(this->reg.samplePosition) & 0x7];
		else
		{
			if (this->reg.psgLastCount != static_cast<uint32_t>(this->reg.samplePosition))
			{
				uint32_t max = static_cast<uint32_t>(this->reg.samplePosition);
				for (uint32_t i = this->reg.psgLastCount; i < max; ++i)
				{
					if (this->reg.psgX & 0x1)
					{
						this->reg.psgX = (this->reg.psgX >> 1) ^ 0x6000;
						this->reg.psgLast = -0x7FFF;
					}
					else
					{
						this->reg.psgX >>= 1;
						this->reg.psgLast = 0x7FFF;
					}
				}

				this->reg.psgLastCount = static_cast<uint32_t>(this->reg.samplePosition);
			}

			return this->reg.psgLast;
		}
	}
}

void Channel::IncrementSample()
{
	double samplePosition = this->reg.samplePosition + this->reg.sampleIncrease;

	if (this->reg.format != 3)
	{
		if (this->reg.samplePosition < 0 && samplePosition >= 0)
		{
			this->ringBuffer.Clear();
			this->ringBuffer.bufferPos += SINC_WIDTH + 1;
			auto preData = std::vector<int16_t>(SINC_WIDTH + 1, this->reg.source->dataptr[0]);
			this->ringBuffer.PushSamples(&preData[0], SINC_WIDTH + 1);
			if (this->reg.totalLength < SINC_WIDTH + 1)
			{
				this->ringBuffer.PushSamples(&this->reg.source->dataptr[0], this->reg.totalLength);
				if (this->reg.repeatMode == 1)
				{
					size_t samplesLeft = SINC_WIDTH + 1 - this->reg.totalLength;
					while (samplesLeft)
					{
						size_t samplesToPush = std::min(samplesLeft, this->reg.length);
						this->ringBuffer.PushSamples(&this->reg.source->dataptr[this->reg.loopStart], samplesToPush);
						samplesLeft -= samplesToPush;
					}
				}
			}
			else
				this->ringBuffer.PushSamples(&this->reg.source->dataptr[0], SINC_WIDTH + 1);
		}
		if (this->reg.samplePosition >= 0)
		{
			uint32_t loc = static_cast<uint32_t>(this->reg.samplePosition) + SINC_WIDTH + 1;
			uint32_t newloc = static_cast<uint32_t>(samplePosition) + SINC_WIDTH + 1;

			if (this->reg.repeatMode == 1)
			{
				while (loc >= this->reg.totalLength)
					loc -= this->reg.length;
				while (newloc >= this->reg.totalLength)
					newloc -= this->reg.length;
			}

			while (loc != newloc)
			{
				this->ringBuffer.NextSample();

				if (loc < this->reg.totalLength)
					this->ringBuffer.PushSample(this->reg.source->dataptr[loc++]);
				else
				{
					++loc;
					this->ringBuffer.PushSample(this->reg.source->dataptr[this->reg.totalLength - 1]);
				}

				if (this->reg.repeatMode == 1 && loc >= this->reg.totalLength)
					loc -= this->reg.length;
			}
		}
	}

	this->reg.samplePosition = samplePosition;

	if (this->reg.format != 3 && this->reg.samplePosition >= this->reg.totalLength)
	{
		if (this->reg.repeatMode == 1)
		{
			while (this->reg.samplePosition >= this->reg.totalLength)
				this->reg.samplePosition -= this->reg.length;
		}
		else
			this->Kill();
	}
}
