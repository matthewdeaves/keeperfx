// KeeperFX sound diagnostic (Apple Silicon / macOS).
//
// A small, standalone, READ-ONLY tool for testers who hear some in-game sounds
// but not others. It does NOT touch the game or write to game data; it only
// reads the sound banks next to the app and prints a report to stdout.
//
// It answers three questions that together isolate a "some sounds missing" fault:
//   1. Is the tester's original DK sound.dat the expected data? (format + sizes
//      of known samples are compared against a known-good baseline)
//   2. Does the tester's OpenAL accept every sample? (each sample is decoded and
//      buffered exactly as the engine does; failures are listed, not fatal)
//   3. Can specific known samples actually be heard? (a handful are played)
//
// The decode/buffer logic here is deliberately a faithful copy of
// src/bflib_sndlib.cpp so that "buffers OK in this tool" means "buffers OK in
// the game". Keep the two in sync if the engine's audio loader changes.
//
// Build:  make -f macos.mk sndcheck        -> bin/keeperfx-sndcheck
// Run:    KeeperFX.app/Contents/MacOS/keeperfx-sndcheck [path-to-game-folder]

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>
#include <stdexcept>

#include <unistd.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

// The MS-ADPCM_SOFT format tokens live in alext.h behind AL_SOFT_MSADPCM. Define
// them defensively so the tool still builds against an OpenAL header set that
// predates the extension (the engine relies on the same tokens).
#ifndef AL_FORMAT_MONO_MSADPCM_SOFT
#define AL_FORMAT_MONO_MSADPCM_SOFT 0x1302
#endif
#ifndef AL_FORMAT_STEREO_MSADPCM_SOFT
#define AL_FORMAT_STEREO_MSADPCM_SOFT 0x1303
#endif

#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_ADPCM 2

// ---- ANSI colour (only when stdout is a terminal) --------------------------
static const char *C_RED = "", *C_GRN = "", *C_YEL = "", *C_DIM = "", *C_RST = "";
static void init_colours() {
	if (isatty(fileno(stdout))) {
		C_RED = "\033[31m"; C_GRN = "\033[32m"; C_YEL = "\033[33m";
		C_DIM = "\033[2m";  C_RST = "\033[0m";
	}
}

// Everything is emitted through logf(): raw (with colour) to stdout, and — with
// ANSI colour stripped — to a plain-text report file the tester can attach. This
// way the on-screen output and the shareable file never drift apart.
static FILE *g_report = nullptr;
static void logf(const char *fmt, ...) {
	char buf[8192];
	va_list ap; va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	if (g_report) {
		for (const char *p = buf; *p;) {
			if (p[0] == '\033' && p[1] == '[') {   // skip a CSI ... 'm' colour code
				p += 2; while (*p && *p != 'm') p++; if (*p) p++;
			} else { fputc(*p++, g_report); }
		}
	}
}

#ifndef SNDCHECK_VERSION
#define SNDCHECK_VERSION "dev"
#endif

// The report is meant to be attached to a public bug report, so abbreviate the
// user's home directory to "~" in any path we print — this keeps the macOS
// username out of the shared output. Used for display only; real file access
// still uses the full path.
static std::string g_home;
static std::string disp(const std::string &path) {
	if (!g_home.empty() && path.rfind(g_home, 0) == 0)
		return std::string("~") + path.substr(g_home.size());
	return path;
}

static uint32_t make_fourcc(const char (&code)[5]) {
	return (uint32_t(uint8_t(code[0])) << 0)  | (uint32_t(uint8_t(code[1])) << 8)
	     | (uint32_t(uint8_t(code[2])) << 16) | (uint32_t(uint8_t(code[3])) << 24);
}

#pragma pack(1)
struct riff_chunk_t { uint32_t tag; uint32_t size; };
struct WAVEFORMATEX {
	uint16_t wFormatTag; uint16_t nChannels; uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec; uint16_t nBlockAlign; uint16_t wBitsPerSample;
};
struct SoundBankHead { uint8_t signature[14]; uint32_t version; };        // 18
struct SoundBankSample {                                                  // 32
	char filename[18];
	uint32_t data_offset; uint32_t sample_rate; uint32_t data_size;
	uint8_t sfxid; uint8_t format_flags;
};
struct SoundBankEntry {                                                   // 16
	uint32_t first_sample_offset; uint32_t first_data_offset;
	uint32_t total_samples_size;  uint32_t entries_count;
};
#pragma pack()

// Decoded WAV plus the AL format the engine would choose for it.
struct decoded_wav {
	uint16_t tag = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	ALenum al_format = 0;
	std::vector<uint8_t> pcm;
	std::string format_label() const {
		char buf[48];
		snprintf(buf, sizeof(buf), "%s ch%u %ubit %uHz",
		         tag == WAVE_FORMAT_ADPCM ? "ADPCM" : "PCM", channels, bits, rate);
		return buf;
	}
};

static bool base_over(size_t off, uint32_t n, size_t total);

// Parse a WAV embedded in the bank at absolute file position `base`. Mirrors
// wave_file() in bflib_sndlib.cpp. Throws on anything malformed.
static decoded_wav parse_wav(const std::vector<uint8_t> &f, size_t base) {
	auto rd = [&](size_t off, void *dst, size_t n) {
		if (off + n > f.size()) throw std::runtime_error("read past end of file");
		memcpy(dst, f.data() + off, n);
	};
	riff_chunk_t riff; rd(base, &riff, sizeof(riff));
	if (riff.tag != make_fourcc("RIFF")) throw std::runtime_error("expected RIFF");
	uint32_t filetype; rd(base + 8, &filetype, 4);
	if (filetype != make_fourcc("WAVE")) throw std::runtime_error("expected WAVE");

	decoded_wav out;
	size_t p = base + 12;
	bool have_fmt = false, have_data = false;
	for (int guard = 0; !(have_fmt && have_data) && guard < 64; ++guard) {
		riff_chunk_t ch; rd(p, &ch, sizeof(ch)); p += sizeof(ch);
		if (ch.tag == make_fourcc("fmt ")) {
			if (ch.size < sizeof(WAVEFORMATEX)) throw std::runtime_error("short fmt chunk");
			WAVEFORMATEX fx; rd(p, &fx, sizeof(fx));
			out.tag = fx.wFormatTag; out.channels = fx.nChannels;
			out.bits = fx.wBitsPerSample; out.rate = fx.nSamplesPerSec;
			if (!(fx.wFormatTag == WAVE_FORMAT_PCM || fx.wFormatTag == WAVE_FORMAT_ADPCM))
				throw std::runtime_error("unsupported wFormatTag");
			if      (fx.nChannels == 1 && fx.wBitsPerSample == 4)  out.al_format = AL_FORMAT_MONO_MSADPCM_SOFT;
			else if (fx.nChannels == 1 && fx.wBitsPerSample == 8)  out.al_format = AL_FORMAT_MONO8;
			else if (fx.nChannels == 1 && fx.wBitsPerSample == 16) out.al_format = AL_FORMAT_MONO16;
			else if (fx.nChannels == 2 && fx.wBitsPerSample == 4)  out.al_format = AL_FORMAT_STEREO_MSADPCM_SOFT;
			else if (fx.nChannels == 2 && fx.wBitsPerSample == 8)  out.al_format = AL_FORMAT_STEREO8;
			else if (fx.nChannels == 2 && fx.wBitsPerSample == 16) out.al_format = AL_FORMAT_STEREO16;
			else throw std::runtime_error("unsupported channel/bit combo");
			p += ch.size;
			have_fmt = true;
		} else if (ch.tag == make_fourcc("data")) {
			if (base_over(p, ch.size, f.size())) throw std::runtime_error("data chunk past end");
			out.pcm.assign(f.begin() + p, f.begin() + p + ch.size);
			have_data = true;
		} else {
			p += ch.size;
		}
	}
	if (!(have_fmt && have_data)) throw std::runtime_error("missing fmt/data chunk");
	return out;
}

static bool base_over(size_t off, uint32_t n, size_t total) { return off + n > total; }

// Load the bank exactly as load_sound_bank() does (directory index 2).
struct bank_sample { std::string name; uint32_t rate, size, wav_base; uint8_t sfxid; };
static std::vector<bank_sample> read_bank_dir(const std::vector<uint8_t> &f) {
	if (f.size() < 4) throw std::runtime_error("file too small");
	uint32_t head_offset; memcpy(&head_offset, f.data() + f.size() - 4, 4);
	size_t off = size_t(head_offset) + sizeof(SoundBankHead);
	SoundBankEntry entries[9];
	if (off + sizeof(entries) > f.size()) throw std::runtime_error("bad header offset");
	memcpy(entries, f.data() + off, sizeof(entries));
	const SoundBankEntry &dir = entries[2];
	if (dir.total_samples_size < sizeof(SoundBankSample))
		throw std::runtime_error("empty sample directory");
	const int count = dir.total_samples_size / sizeof(SoundBankSample);
	std::vector<bank_sample> out;
	out.reserve(count);
	for (int i = 0; i < count; ++i) {
		size_t so = size_t(dir.first_sample_offset) + sizeof(SoundBankSample) * i;
		if (so + sizeof(SoundBankSample) > f.size()) break;
		SoundBankSample s; memcpy(&s, f.data() + so, sizeof(s));
		char nm[19]; memcpy(nm, s.filename, 18); nm[18] = 0;
		out.push_back({std::string(nm), s.sample_rate, s.data_size,
		               uint32_t(dir.first_data_offset) + s.data_offset, s.sfxid});
	}
	return out;
}

static std::vector<uint8_t> read_whole_file(const std::string &path) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) return {};
	fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
	std::vector<uint8_t> buf(n > 0 ? n : 0);
	if (n > 0 && fread(buf.data(), 1, n, fp) != size_t(n)) buf.clear();
	fclose(fp);
	return buf;
}

// Known-good baseline captured from a verified DK-Gold (GOG) install on Apple
// Silicon. Index, filename, expected format label, expected byte size.
struct baseline_row { int idx; const char *name; const char *fmt; uint32_t size; const char *what; };
static const baseline_row BASELINE[] = {
	{  63, "dig1.wav",    "PCM ch1 16bit 22050Hz", 35760, "imp digging (works for testers)" },
	{ 118, "digmark.wav", "PCM ch1 16bit 22050Hz", 19888, "dig-mark 'shovel'" },
	{ 119, "cant.wav",    "PCM ch1 16bit 22050Hz",  1696, "refusal" },
	{ 850, "build1.wav",  "PCM ch1 16bit 22050Hz", 34080, "put-down room" },
	{ 851, "build2.wav",  "PCM ch1 16bit 22050Hz", 21344, "put-down room" },
	{ 852, "build3.wav",  "PCM ch1 16bit 22050Hz", 30864, "put-down room" },
	{ 150, "beat1.wav",   "PCM ch1 16bit 22050Hz", 49008, "dungeon-heart beat" },
};

// Replicate macos_chdir_to_bundle_parent() so, when launched from inside the
// .app, the tool finds the sibling game data — same behaviour as the engine.
static void chdir_to_bundle_parent() {
	char exe[PATH_MAX]; uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) != 0) return;
	char resolved[PATH_MAX];
	if (!realpath(exe, resolved)) return;
	char *marker = strstr(resolved, "/Contents/MacOS/");
	if (!marker) return;
	*marker = 0;
	char *slash = strrchr(resolved, '/');
	if (!slash) return;
	*slash = 0;
	if (chdir(resolved) != 0) return;
}

// Convert an AL_FORMAT_MONO_MSADPCM_SOFT buffer to MONO8 the way the engine does
// (the "heart6a.wav" path). Not a real ADPCM decode — mirrored only so the
// buffer test matches the engine byte-for-byte.
static std::vector<uint8_t> engine_msadpcm_mono_hack(const std::vector<uint8_t> &pcm) {
	std::vector<uint8_t> out(pcm.size() * 2);
	for (size_t i = 0; i < pcm.size(); ++i) {
		out[(i * 2) + 0] = (pcm[i] >> 4) * 2;
		out[(i * 2) + 1] = (pcm[i] & 0x7) * 2;
	}
	return out;
}

// Read a small sysctl string (e.g. "kern.osproductversion", "hw.model").
static std::string sysctl_str(const char *name) {
	size_t n = 0;
	if (sysctlbyname(name, nullptr, &n, nullptr, 0) != 0 || n == 0) return "?";
	std::string v(n, 0);
	if (sysctlbyname(name, &v[0], &n, nullptr, 0) != 0) return "?";
	if (!v.empty() && v.back() == 0) v.pop_back();
	return v;
}

// Open the shareable report file. Try next to the game data first (most
// discoverable), then the Desktop, then $HOME, then /tmp. Returns the path used.
static std::string open_report(const std::string &game_dir) {
	const char *home = getenv("HOME");
	std::vector<std::string> candidates = { game_dir + "keeperfx-soundcheck.txt" };
	if (home) {
		candidates.push_back(std::string(home) + "/Desktop/keeperfx-soundcheck.txt");
		candidates.push_back(std::string(home) + "/keeperfx-soundcheck.txt");
	}
	candidates.push_back("/tmp/keeperfx-soundcheck.txt");
	for (const auto &path : candidates) {
		FILE *fp = fopen(path.c_str(), "wb");
		if (fp) { g_report = fp; return path; }
	}
	return "";
}

static void print_openal_devices() {
	if (!alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT")) return;
	const char *devs = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);
	const char *def  = alcGetString(nullptr, ALC_DEFAULT_ALL_DEVICES_SPECIFIER);
	if (!devs || !*devs) return;
	logf("  output devices seen by OpenAL:\n");
	for (const char *d = devs; *d; d += strlen(d) + 1)
		logf("    - %s%s\n", d, (def && strcmp(d, def) == 0) ? "   (default)" : "");
}

// Print any keeperfx.cfg lines that affect audio, so a muted/disabled config is
// visible. Read-only.
static void print_cfg_audio(const std::string &game_dir) {
	auto cfg = read_whole_file(game_dir + "keeperfx.cfg");
	if (cfg.empty()) { logf("keeperfx.cfg : not found\n"); return; }
	std::string text(cfg.begin(), cfg.end());
	logf("keeperfx.cfg audio settings:\n");
	size_t start = 0; bool any = false;
	while (start < text.size()) {
		size_t nl = text.find('\n', start);
		std::string line = text.substr(start, (nl == std::string::npos ? text.size() : nl) - start);
		start = (nl == std::string::npos) ? text.size() : nl + 1;
		std::string low = line;
		for (auto &c : low) c = tolower((unsigned char)c);
		if (low.find("sound") != std::string::npos || low.find("music") != std::string::npos ||
		    low.find("volume") != std::string::npos || low.find("atmos") != std::string::npos) {
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			if (!line.empty() && line[0] != ';') { logf("    %s\n", line.c_str()); any = true; }
		}
	}
	if (!any) logf("    (no sound/music/volume lines found)\n");
}

int main(int argc, char *argv[]) {
	init_colours();
	if (const char *h = getenv("HOME")) g_home = h;

	std::string game_dir;
	if (argc > 1) {
		game_dir = argv[1];
	} else {
		chdir_to_bundle_parent();
		char cwd[PATH_MAX]; if (getcwd(cwd, sizeof(cwd))) game_dir = cwd;
	}
	if (!game_dir.empty() && game_dir.back() != '/') game_dir += '/';

	std::string report_path = open_report(game_dir);

	logf("KeeperFX sound diagnostic (read-only)  v%s\n", SNDCHECK_VERSION);
	logf("=====================================\n");
	time_t now = time(nullptr);
	char ts[64] = "?"; struct tm tmv;
	if (localtime_r(&now, &tmv)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
	struct utsname un; memset(&un, 0, sizeof(un)); uname(&un);
	logf("when     : %s\n", ts);
	logf("macOS    : %s (kernel %s %s, %s)\n",
	     sysctl_str("kern.osproductversion").c_str(), un.sysname, un.release, un.machine);
	logf("model    : %s\n", sysctl_str("hw.model").c_str());
	if (!report_path.empty()) logf("report   : %s\n", disp(report_path).c_str());
	else logf("report   : %s(could not open a report file; copy this text instead)%s\n", C_YEL, C_RST);
	logf("\n");
	logf("Game folder : %s\n", disp(game_dir).c_str());

	std::string snd_path = game_dir + "sound/sound.dat";
	auto snd = read_whole_file(snd_path);
	if (snd.empty()) {
		logf("%serror%s: could not read %s\n", C_RED, C_RST, snd_path.c_str());
		logf("Run the tool from next to your game data, or pass the game folder:\n");
		logf("    keeperfx-sndcheck /path/to/your/keeperfx-folder\n");
		return 2;
	}
	logf("sound.dat   : %s (%zu bytes)\n", disp(snd_path).c_str(), snd.size());

	// speech bank (english fallback), reported for completeness
	for (const char *cand : {"sound/speech_eng.dat", "sound/speech.dat"}) {
		auto sp = read_whole_file(game_dir + cand);
		if (!sp.empty()) { logf("speech      : %s (%zu bytes)\n", disp(game_dir + cand).c_str(), sp.size()); break; }
	}
	logf("\n");
	print_cfg_audio(game_dir);
	logf("\n");

	// -- parse the bank ------------------------------------------------------
	std::vector<bank_sample> samples;
	try { samples = read_bank_dir(snd); }
	catch (const std::exception &e) {
		logf("%serror%s: sound.dat is not a valid bank: %s\n", C_RED, C_RST, e.what());
		return 3;
	}
	logf("Samples in bank: %zu\n\n", samples.size());

	// decode each once; keep the decoded form for the buffer + histogram passes
	struct entry { bank_sample meta; decoded_wav wav; std::string err; };
	std::vector<entry> decoded(samples.size());
	int n_bad = 0, n_empty = 0;
	// histogram
	struct { const char *label; int count; } hist[8] = {
		{"PCM mono 16",0},{"PCM stereo 16",0},{"PCM mono 8",0},{"PCM stereo 8",0},
		{"MS-ADPCM mono",0},{"MS-ADPCM stereo",0},{"empty",0},{"other/bad",0} };
	for (size_t i = 0; i < samples.size(); ++i) {
		decoded[i].meta = samples[i];
		if (samples[i].size == 0) { decoded[i].err = "empty"; hist[6].count++; n_empty++; continue; }
		try {
			decoded[i].wav = parse_wav(snd, samples[i].wav_base);
			const auto &w = decoded[i].wav;
			int b = 7;
			if      (w.al_format == AL_FORMAT_MONO16)             b = 0;
			else if (w.al_format == AL_FORMAT_STEREO16)           b = 1;
			else if (w.al_format == AL_FORMAT_MONO8)              b = 2;
			else if (w.al_format == AL_FORMAT_STEREO8)            b = 3;
			else if (w.al_format == AL_FORMAT_MONO_MSADPCM_SOFT)  b = 4;
			else if (w.al_format == AL_FORMAT_STEREO_MSADPCM_SOFT)b = 5;
			hist[b].count++;
		} catch (const std::exception &e) {
			decoded[i].err = e.what(); hist[7].count++; n_bad++;
		}
	}
	logf("Format breakdown:\n");
	for (auto &h : hist) if (h.count) logf("  %-16s %5d\n", h.label, h.count);
	if (n_bad)   logf("  %s%d sample(s) failed to parse%s\n", C_RED, n_bad, C_RST);
	if (n_empty) logf("  %s%d sample(s) are empty%s\n", C_YEL, n_empty, C_RST);
	logf("\n");

	// -- known-ID baseline comparison ---------------------------------------
	logf("Known samples vs. expected DK-Gold baseline:\n");
	logf("  %-4s %-13s %-24s %-9s %s\n", "id", "file", "format", "size", "result");
	int mismatches = 0;
	for (const auto &bl : BASELINE) {
		if (bl.idx < 0 || bl.idx >= (int)decoded.size()) {
			logf("  %s%-4d %-13s MISSING (bank has only %zu samples)%s\n",
			       C_RED, bl.idx, bl.name, decoded.size(), C_RST);
			mismatches++; continue;
		}
		const auto &e = decoded[bl.idx];
		std::string got_fmt = e.err.empty() ? e.wav.format_label() : ("ERR:" + e.err);
		bool name_ok = (e.meta.name == bl.name);
		bool fmt_ok  = (got_fmt == bl.fmt);
		bool size_ok = (bl.size == 0) || (e.meta.size == bl.size);
		bool ok = name_ok && fmt_ok && size_ok;
		if (!ok) mismatches++;
		logf("  %-4d %-13s %-24s %-9u %s%s%s%s\n",
		       bl.idx, e.meta.name.c_str(), got_fmt.c_str(), e.meta.size,
		       ok ? C_GRN : C_RED, ok ? "OK" : "MISMATCH",
		       name_ok ? "" : " name", C_RST);
	}
	logf("  %sexpected file for each id: %s%s\n", C_DIM,
	       "63 dig1 / 118 digmark / 119 cant / 150 beat1 / 850-852 build", C_RST);
	logf("\n");

	// -- OpenAL environment --------------------------------------------------
	logf("OpenAL environment:\n");
	ALCdevice *dev = alcOpenDevice(nullptr);
	if (!dev) {
		logf("  %serror%s: cannot open an OpenAL output device on this machine.\n", C_RED, C_RST);
		logf("  This alone would silence ALL game sound. Check macOS audio output/permissions.\n");
		return 4;
	}
	ALCcontext *ctx = alcCreateContext(dev, nullptr);
	if (!ctx || alcMakeContextCurrent(ctx) == ALC_FALSE) {
		logf("  %serror%s: cannot create/activate an OpenAL context.\n", C_RED, C_RST);
		return 4;
	}
	const char *devname = alcGetString(dev, ALC_ALL_DEVICES_SPECIFIER);
	if (!devname || !*devname) devname = alcGetString(dev, ALC_DEVICE_SPECIFIER);
	logf("  device   : %s\n", devname ? devname : "(unknown)");
	logf("  vendor   : %s\n", alGetString(AL_VENDOR));
	logf("  renderer : %s\n", alGetString(AL_RENDERER));
	logf("  version  : %s\n", alGetString(AL_VERSION));
	bool has_adpcm = alIsExtensionPresent("AL_SOFT_MSADPCM");
	logf("  AL_SOFT_MSADPCM: %s%s%s\n",
	       has_adpcm ? C_GRN : C_DIM, has_adpcm ? "present" : "absent (informational; not used by the engine)", C_RST);
	print_openal_devices();
	logf("\n");

	// -- buffer test: decode+buffer every sample as the engine would ---------
	logf("Buffer test (decode + upload each sample to OpenAL):\n");
	int ok_count = 0, fail_count = 0;
	std::vector<std::string> failures;
	for (auto &e : decoded) {
		if (!e.err.empty()) { fail_count++; failures.push_back(e.meta.name + " (" + e.err + ")"); continue; }
		alGetError();
		ALuint buf = 0; alGenBuffers(1, &buf);
		bool ok = true; std::string why;
		try {
			const auto &w = e.wav;
			if (w.al_format == AL_FORMAT_MONO_MSADPCM_SOFT) {
				auto conv = engine_msadpcm_mono_hack(w.pcm);
				alBufferData(buf, AL_FORMAT_MONO8, conv.data(), conv.size(), w.rate);
			} else if (w.al_format == AL_FORMAT_STEREO_MSADPCM_SOFT) {
				ok = false; why = "stereo MS-ADPCM not implemented by engine";
			} else {
				alBufferData(buf, w.al_format, w.pcm.data(), (ALsizei)w.pcm.size(), w.rate);
			}
			ALenum err = alGetError();
			if (ok && err != AL_NO_ERROR) { ok = false; why = std::string("alBufferData: ") + alGetString(err); }
		} catch (const std::exception &ex) { ok = false; why = ex.what(); }
		alDeleteBuffers(1, &buf);
		if (ok) ok_count++;
		else { fail_count++; failures.push_back(e.meta.name + " (" + why + ")"); }
	}
	logf("  %s%d ok%s", C_GRN, ok_count, C_RST);
	if (fail_count) logf(", %s%d failed%s", C_RED, fail_count, C_RST);
	logf(" of %zu\n", decoded.size());
	for (size_t i = 0; i < failures.size() && i < 40; ++i)
		logf("    %sFAIL%s %s\n", C_RED, C_RST, failures[i].c_str());
	if (failures.size() > 40) logf("    ... and %zu more\n", failures.size() - 40);
	logf("\n");

	// -- audible playback of the key samples ---------------------------------
	logf("Playing key samples (listen now)...\n");
	auto play_one = [&](const char *label, int idx) {
		if (idx < 0 || idx >= (int)decoded.size() || !decoded[idx].err.empty()) {
			logf("  %s(cannot play %s)%s\n", C_YEL, label, C_RST); return;
		}
		const auto &w = decoded[idx].wav;
		alGetError();
		ALuint buf = 0; alGenBuffers(1, &buf);
		if (w.al_format == AL_FORMAT_MONO_MSADPCM_SOFT) {
			auto conv = engine_msadpcm_mono_hack(w.pcm);
			alBufferData(buf, AL_FORMAT_MONO8, conv.data(), conv.size(), w.rate);
		} else if (w.al_format == AL_FORMAT_STEREO_MSADPCM_SOFT) {
			logf("  %s(skip %s: unsupported format)%s\n", C_YEL, label, C_RST);
			alDeleteBuffers(1, &buf); return;
		} else {
			alBufferData(buf, w.al_format, w.pcm.data(), (ALsizei)w.pcm.size(), w.rate);
		}
		if (alGetError() != AL_NO_ERROR) { logf("  %s(buffer failed: %s)%s\n", C_RED, label, C_RST); alDeleteBuffers(1,&buf); return; }
		ALuint src = 0; alGenSources(1, &src);
		alSourcei(src, AL_BUFFER, buf);
		logf("  > %-26s ", label); fflush(stdout);
		alSourcePlay(src);
		// wait until it stops, with a safety cap
		for (int ms = 0; ms < 4000; ms += 25) {
			ALint st = 0; alGetSourcei(src, AL_SOURCE_STATE, &st);
			if (st != AL_PLAYING) break;
			usleep(25 * 1000);
		}
		usleep(250 * 1000);
		logf("done\n");
		alDeleteSources(1, &src);
		alDeleteBuffers(1, &buf);
	};
	play_one("digmark.wav (dig-mark)", 118);
	play_one("cant.wav (refusal)", 119);
	play_one("build1.wav (put-down)", 850);
	play_one("dig1.wav (imp dig)", 63);
	play_one("beat1.wav (dungeon-heart beat)", 150);
	logf("\n");

	// -- verdict -------------------------------------------------------------
	logf("Summary\n-------\n");
	logf("  samples parsed ok : %zu / %zu\n", decoded.size() - n_bad - n_empty, decoded.size());
	logf("  baseline mismatches: %s%d%s\n", mismatches ? C_RED : C_GRN, mismatches, C_RST);
	logf("  buffer failures    : %s%d%s\n", fail_count ? C_RED : C_GRN, fail_count, C_RST);
	if (mismatches == 0 && fail_count == 0) {
		logf("\n  %sAll samples load and buffer correctly on this machine.%s\n", C_GRN, C_RST);
		logf("  If you still don't hear some sounds in-game, note WHICH of the 5 played\n");
		logf("  samples above you did/didn't hear and share this whole output.\n");
	} else {
		logf("\n  %sProblems found above.%s Mismatches point to different/partial game data;\n", C_YEL, C_RST);
		logf("  buffer failures point to an OpenAL/data issue. Share this whole output.\n");
	}

	if (!report_path.empty())
		logf("\n  A copy of this report was saved to:\n    %s\n  Attach that file (or paste this text) when reporting.\n", disp(report_path).c_str());

	alcMakeContextCurrent(nullptr);
	alcDestroyContext(ctx);
	alcCloseDevice(dev);
	if (g_report) fclose(g_report);
	return 0;
}
