// SPDX-FileCopyrightText: 2026 Sergilol88
// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFont>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QPushButton>
#include <QString>
#include <QStandardPaths>
#include <QTableWidget>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("eb-recorder", "en-US")

namespace {
constexpr const char *kVersion = "0.3.2";
constexpr const char *kEbEncoderPrefix = "multitrack video video encoder ";
constexpr const char *kEbLiveAudioEncoderName = "multitrack video live audio 0";
constexpr const char *kEbVodAudioEncoderName = "multitrack video vod audio 0";
constexpr const char *kLocalOutputName = "eb-recorder local output";
constexpr const char *kLocalOutputType = "ffmpeg_muxer";
constexpr const char *kStartHotkeyName = "EBRecorder.StartRecording";
constexpr const char *kStopHotkeyName = "EBRecorder.StopRecording";
// Keep Matroska clusters short so an interrupted session leaves only a small
// tail at risk.  The MKV container itself does not depend on a final MOOV-like
// index to remain readable after an abrupt process/power loss.
constexpr const char *kMuxerSettings = "cluster_time_limit=1000";
// Timers are intentionally coarse and low-frequency. They are only used for
// lightweight state discovery; the plugin never touches display timing, VRR,
// G-SYNC, refresh-rate or Windows/NVIDIA display configuration APIs.
constexpr int kAutoStartRetryMs = 500;
constexpr int kAutoStartTimeoutMs = 15000;
constexpr int kDialogRefreshMs = 1000;
constexpr qint64 kMatroskaHeaderScanBytes = 2 * 1024 * 1024;

struct EncoderInfo {
	int index = -1;
	std::string name;
	std::string codec;
	uint32_t width = 0;
	uint32_t height = 0;
	int64_t bitrate = 0;
	bool active = false;
};

struct AudioEncoderInfo {
	int64_t bitrate = 0;
	bool active = false;
};

enum class AudioSelection {
	Live = 0,
	Vod = 1,
	Both = 2,
};

enum class RecordingUiState {
	Idle,
	Recording,
	Stopping,
	Stopped,
	Error,
};

obs_output_t *g_recordingOutput = nullptr;
QString g_recordingPath;
QString g_recordingError;
RecordingUiState g_recordingUiState = RecordingUiState::Idle;
bool g_frontendCallbackRegistered = false;
bool g_obsExiting = false;

QString g_settingsPath;
bool g_autoRecordEnabled = false;
AudioSelection g_audioSelection = AudioSelection::Live;
std::vector<int64_t> g_recordingAudioBitratesBps;
bool g_audioBitrateMetadataPending = false;
bool g_cleanStopRequested = false;
bool g_autoStartPending = false;
qint64 g_autoStartDeadlineMs = 0;
QTimer *g_autoStartTimer = nullptr;
obs_hotkey_pair_id g_recordingHotkeys = OBS_INVALID_HOTKEY_PAIR_ID;

QString fallbackText(const char *key)
{
	const char *locale = obs_get_locale();
	const bool russian = locale && std::strncmp(locale, "ru", 2) == 0;

	struct Translation {
		const char *key;
		const char *en;
		const char *ru;
	};

	static const Translation translations[] = {
		{"EBRecorder.Title", "EB Recorder 0.3.2", "EB Recorder 0.3.2"},
		{"EBRecorder.Intro",
		 "Version 0.3.2 records the active highest-resolution Enhanced Broadcasting rendition to crash-resilient Matroska (MKV) by reusing the existing EB video and selected Live/VOD audio encoders. Recording can be started manually or automatically together with the EB stream.",
		 "Версия 0.3.2 записывает активный поток Enhanced Broadcasting с максимальным разрешением в устойчивый к аварийному завершению Matroska (MKV), повторно используя уже работающий EB-видеокодировщик и выбранные аудиодорожки Live/VOD. Запись можно запускать вручную или автоматически вместе с EB-трансляцией."},
		{"EBRecorder.Status.None",
		 "No active Enhanced Broadcasting video encoders detected. Start an EB stream and refresh.",
		 "Активные видеокодировщики Enhanced Broadcasting не найдены. Запусти EB-трансляцию и обнови список."},
		{"EBRecorder.Status.Found", "Detected %1 active Enhanced Broadcasting video encoder(s).",
		 "Обнаружено активных видеокодировщиков Enhanced Broadcasting: %1."},
		{"EBRecorder.Selected.None", "Top EB stream: not available.", "Верхний EB-поток: недоступен."},
		{"EBRecorder.Selected.Found", "Top EB stream: encoder %1 — %2, %3×%4, %5 kbps.",
		 "Верхний EB-поток: кодировщик %1 — %2, %3×%4, %5 кбит/с."},
		{"EBRecorder.Audio.Status", "EB audio — Live: %1; VOD: %2. Recording selection: %3.",
		 "EB-аудио — Live: %1; VOD: %2. Для записи выбрано: %3."},
		{"EBRecorder.Audio.ActiveWithBitrate", "active, %1 kbps", "активно, %1 кбит/с"},
		{"EBRecorder.Audio.Active", "active", "активно"},
		{"EBRecorder.Audio.Inactive", "not active", "не активно"},
		{"EBRecorder.Audio.VodDisabled", "disabled in OBS output settings", "отключено в настройках вывода OBS"},
		{"EBRecorder.Audio.VodWaiting", "enabled in OBS, waiting for EB", "включено в OBS, ожидает запуска EB"},
		{"EBRecorder.AudioSelection", "Audio tracks", "Аудиодорожки"},
		{"EBRecorder.AudioSelection.Live", "Live", "Live"},
		{"EBRecorder.AudioSelection.Vod", "VOD", "VOD"},
		{"EBRecorder.AudioSelection.Both", "Live + VOD", "Live + VOD"},
		{"EBRecorder.AudioSelection.Tooltip",
		 "Choose which existing Enhanced Broadcasting audio encoder(s) are reused in the local MKV. VOD choices are disabled when the current OBS output settings cannot create a separate Twitch VOD Track. Changing this option affects the next recording.",
		 "Выбери, какие уже работающие аудиокодировщики Enhanced Broadcasting использовать в локальном MKV. Варианты с VOD недоступны, если текущие настройки вывода OBS не могут создать отдельную дорожку Twitch VOD. Изменение применяется к следующей записи."},
		{"EBRecorder.AudioSelection.VodUnavailable",
		 "Enable Twitch VOD Track in OBS Settings → Output. In Advanced mode, the Live and VOD tracks must be different.",
		 "Включи «Дорожка Twitch VOD» в OBS → Настройки → Вывод. В расширенном режиме Live и VOD должны использовать разные дорожки."},
		{"EBRecorder.Recording.Idle", "Local EB recording: stopped.", "Локальная EB-запись: остановлена."},
		{"EBRecorder.Recording.Active", "Local EB recording: RECORDING → %1",
		 "Локальная EB-запись: ИДЁТ → %1"},
		{"EBRecorder.Recording.Stopping", "Local EB recording: stopping… → %1",
		 "Локальная EB-запись: останавливается… → %1"},
		{"EBRecorder.Recording.Stopped", "Local EB recording stopped. Last file: %1",
		 "Локальная EB-запись остановлена. Последний файл: %1"},
		{"EBRecorder.Recording.Error", "Local EB recording error: %1", "Ошибка локальной EB-записи: %1"},
		{"EBRecorder.Recording.AutoPending", "Automatic recording: waiting for active EB encoders…",
		 "Автозапись: ожидаю активные EB-кодировщики…"},
		{"EBRecorder.Settings", "Settings", "Настройки"},
		{"EBRecorder.AutoRecord", "Automatically start recording with the EB stream",
		 "Автоматически начинать запись при запуске EB-трансляции"},
		{"EBRecorder.AutoRecord.Tooltip",
		 "When enabled, EB Recorder starts one local recording after each EB stream successfully starts. Manually stopping that recording does not start it again until the next stream. Disabling this option does not stop a recording that is already running.",
		 "Если включено, EB Recorder один раз автоматически запускает локальную запись после успешного старта каждой EB-трансляции. Если остановить такую запись вручную, она не запустится снова до следующего стрима. Отключение этой настройки не останавливает уже идущую запись."},
		{"EBRecorder.Error.AlreadyActive", "The local EB output is already active.",
		 "Локальная EB-запись уже активна."},
		{"EBRecorder.Error.NoTop", "No active TOP EB video encoder is available.",
		 "Нет активного верхнего EB-видеокодировщика."},
		{"EBRecorder.Error.NoAudio", "Required EB audio track(s) are not active for the selected mode: %1.",
		 "Для выбранного режима не активны необходимые EB-аудиодорожки: %1."},
		{"EBRecorder.Error.OutputCreate", "Could not create the Matroska recording output.", "Не удалось создать Matroska output для записи."},
		{"EBRecorder.Error.AttachReuse", "OBS did not attach the existing EB encoder objects to the local output.",
		 "OBS не подключил существующие EB-кодировщики к локальному output."},
		{"EBRecorder.Error.OutputStart", "OBS could not start the local Matroska output%1.",
		 "OBS не смог запустить локальный Matroska output%1."},
		{"EBRecorder.Error.Path", "Could not prepare the recording folder.", "Не удалось подготовить папку записи."},
		{"EBRecorder.Col.Index", "Index", "Индекс"},
		{"EBRecorder.Col.Codec", "Codec", "Кодек"},
		{"EBRecorder.Col.Resolution", "Resolution", "Разрешение"},
		{"EBRecorder.Col.Bitrate", "Bitrate", "Битрейт"},
		{"EBRecorder.Col.Active", "Active", "Активен"},
		{"EBRecorder.Col.Role", "Role", "Роль"},
		{"EBRecorder.Col.Name", "OBS encoder name", "Имя кодировщика OBS"},
		{"EBRecorder.Yes", "Yes", "Да"},
		{"EBRecorder.No", "No", "Нет"},
		{"EBRecorder.Top", "TOP", "ВЕРХНИЙ"},
		{"EBRecorder.Refresh", "Refresh", "Обновить"},
		{"EBRecorder.StartRecording", "Start recording", "Начать запись"},
		{"EBRecorder.StopRecording", "Stop recording", "Остановить запись"},
		{"EBRecorder.Close", "Close", "Закрыть"},
		{"EBRecorder.Hotkey.Start", "EB Recorder: Start recording", "EB Recorder: Начать запись"},
		{"EBRecorder.Hotkey.Stop", "EB Recorder: Stop recording", "EB Recorder: Остановить запись"},
	};

	for (const auto &entry : translations) {
		if (std::strcmp(key, entry.key) == 0)
			return QString::fromUtf8(russian ? entry.ru : entry.en);
	}

	return QString::fromUtf8(key ? key : "");
}

QString ebTr(const char *key)
{
	const char *text = obs_module_text(key);
	if (text && std::strcmp(text, key) != 0)
		return QString::fromUtf8(text);

	return fallbackText(key);
}

void logLocaleDiagnostics()
{
	obs_module_t *module = obs_current_module();
	const char *locale = obs_get_locale();
	const char *binaryPath = obs_get_module_binary_path(module);
	const char *dataPath = obs_get_module_data_path(module);

	blog(LOG_INFO, "[EB Recorder] locale=%s", locale ? locale : "<null>");
	blog(LOG_INFO, "[EB Recorder] module binary path: %s", binaryPath ? binaryPath : "<null>");
	blog(LOG_INFO, "[EB Recorder] module data path: %s", dataPath ? dataPath : "<null>");

	char *enLocalePath = obs_module_file("locale/en-US.ini");
	char *ruLocalePath = obs_module_file("locale/ru-RU.ini");
	blog(LOG_INFO, "[EB Recorder] locale/en-US.ini: %s", enLocalePath ? enLocalePath : "NOT FOUND");
	blog(LOG_INFO, "[EB Recorder] locale/ru-RU.ini: %s", ruLocalePath ? ruLocalePath : "NOT FOUND");

	const char *translatedTitle = nullptr;
	const bool titleFound = obs_module_get_string("EBRecorder.Title", &translatedTitle);
	blog(LOG_INFO, "[EB Recorder] OBS locale lookup: %s%s%s", titleFound ? "OK" : "FAILED",
	     titleFound && translatedTitle ? " -> " : "", titleFound && translatedTitle ? translatedTitle : "");

	if (enLocalePath)
		bfree(enLocalePath);
	if (ruLocalePath)
		bfree(ruLocalePath);
}

QString codecLabel(const std::string &codec)
{
	if (codec == "h264")
		return QStringLiteral("H.264");
	if (codec == "hevc")
		return QStringLiteral("HEVC");
	if (codec == "av1")
		return QStringLiteral("AV1");
	return QString::fromStdString(codec);
}

int parseEncoderIndex(const char *name)
{
	if (!name)
		return -1;

	const size_t prefixLength = std::strlen(kEbEncoderPrefix);
	if (std::strncmp(name, kEbEncoderPrefix, prefixLength) != 0)
		return -1;

	const char *first = name + prefixLength;
	const char *last = name + std::strlen(name);
	if (first == last)
		return -1;

	int index = -1;
	const auto result = std::from_chars(first, last, index);
	if (result.ec != std::errc{} || result.ptr != last || index < 0)
		return -1;

	return index;
}

bool collectEncoder(void *param, obs_encoder_t *encoder)
{
	auto *encoders = static_cast<std::vector<EncoderInfo> *>(param);
	if (!encoder || !encoders)
		return true;

	const char *name = obs_encoder_get_name(encoder);
	const int index = parseEncoderIndex(name);
	if (index < 0)
		return true;

	EncoderInfo info;
	info.index = index;
	info.name = name ? name : "";
	const char *codec = obs_encoder_get_codec(encoder);
	info.codec = codec ? codec : "unknown";
	info.width = obs_encoder_get_width(encoder);
	info.height = obs_encoder_get_height(encoder);
	info.active = obs_encoder_active(encoder);

	obs_data_t *settings = obs_encoder_get_settings(encoder);
	if (settings) {
		info.bitrate = obs_data_get_int(settings, "bitrate");
		obs_data_release(settings);
	}

	encoders->push_back(std::move(info));
	return true;
}

std::vector<EncoderInfo> getEbEncoders()
{
	std::vector<EncoderInfo> result;
	result.reserve(4);
	obs_enum_encoders(collectEncoder, &result);
	std::sort(result.begin(), result.end(), [](const EncoderInfo &a, const EncoderInfo &b) {
		return a.index < b.index;
	});
	return result;
}

const EncoderInfo *findTopEncoder(const std::vector<EncoderInfo> &encoders)
{
	const EncoderInfo *best = nullptr;
	uint64_t bestPixels = 0;

	for (const auto &encoder : encoders) {
		if (!encoder.active)
			continue;

		const uint64_t pixels = static_cast<uint64_t>(encoder.width) * encoder.height;
		if (!best || pixels > bestPixels ||
		    (pixels == bestPixels && encoder.bitrate > best->bitrate)) {
			best = &encoder;
			bestPixels = pixels;
		}
	}

	return best;
}

QString makeSignature(const std::vector<EncoderInfo> &encoders, const EncoderInfo *top)
{
	QStringList parts;
	for (const auto &encoder : encoders) {
		parts << QStringLiteral("%1:%2:%3x%4:%5:%6")
				 .arg(encoder.index)
				 .arg(QString::fromStdString(encoder.codec))
				 .arg(encoder.width)
				 .arg(encoder.height)
				 .arg(encoder.bitrate)
				 .arg(encoder.active ? 1 : 0);
	}
	parts << QStringLiteral("top=%1").arg(top ? top->index : -1);
	return parts.join('|');
}

AudioEncoderInfo getAudioEncoderInfo(const char *name)
{
	AudioEncoderInfo info;
	obs_encoder_t *audio = name ? obs_get_encoder_by_name(name) : nullptr;
	if (!audio)
		return info;

	info.active = obs_encoder_active(audio);
	if (info.active) {
		obs_data_t *settings = obs_encoder_get_settings(audio);
		if (settings) {
			info.bitrate = obs_data_get_int(settings, "bitrate");
			obs_data_release(settings);
		}
	}

	obs_encoder_release(audio);
	return info;
}

struct EbAudioState {
	AudioEncoderInfo live;
	AudioEncoderInfo vod;
};

struct VodTrackConfigState {
	bool known = false;
	bool configured = false;
	int liveTrack = 0;
	int vodTrack = 0;
};

EbAudioState getEbAudioState()
{
	return {getAudioEncoderInfo(kEbLiveAudioEncoderName), getAudioEncoderInfo(kEbVodAudioEncoderName)};
}

VodTrackConfigState getObsVodTrackConfigState()
{
	VodTrackConfigState state;
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return state;

	const char *mode = config_get_string(config, "Output", "Mode");
	if (!mode || !*mode)
		return state;

	state.known = true;
	const bool advancedOutput = std::strcmp(mode, "Advanced") == 0;

	if (advancedOutput) {
		const bool enabled = config_get_bool(config, "AdvOut", "VodTrackEnabled");
		state.liveTrack = static_cast<int>(config_get_int(config, "AdvOut", "TrackIndex"));
		state.vodTrack = static_cast<int>(config_get_int(config, "AdvOut", "VodTrackIndex"));
		const bool distinctTracks =
			state.liveTrack > 0 && state.vodTrack > 0 && state.liveTrack != state.vodTrack;
		state.configured = enabled && distinctTracks;
	} else {
		// This mirrors OBS SimpleOutput::IsVodTrackEnabled() apart from the
		// service capability check. EB Recorder is only useful with Twitch EB;
		// once streaming starts, the real VOD encoder remains the final truth.
		const bool simpleAdvanced = config_get_bool(config, "SimpleOutput", "UseAdvanced");
		const bool enabled = config_get_bool(config, "SimpleOutput", "VodTrackEnabled");
		state.configured = simpleAdvanced && enabled;
	}

	return state;
}

bool vodSelectionAllowed(const EbAudioState &audio, const VodTrackConfigState &config)
{
	// While EB is live, the actual active encoder is authoritative. Before EB
	// starts, use the current OBS profile settings to prevent impossible choices.
	if (audio.vod.active)
		return true;
	if (!config.known)
		return true;
	return config.configured;
}

QString audioSelectionKey(AudioSelection selection)
{
	switch (selection) {
	case AudioSelection::Vod:
		return QStringLiteral("vod");
	case AudioSelection::Both:
		return QStringLiteral("both");
	case AudioSelection::Live:
	default:
		return QStringLiteral("live");
	}
}

AudioSelection audioSelectionFromKey(const QString &key)
{
	if (key.compare(QStringLiteral("vod"), Qt::CaseInsensitive) == 0)
		return AudioSelection::Vod;
	if (key.compare(QStringLiteral("both"), Qt::CaseInsensitive) == 0)
		return AudioSelection::Both;
	return AudioSelection::Live;
}

QString audioSelectionLabel(AudioSelection selection)
{
	switch (selection) {
	case AudioSelection::Vod:
		return ebTr("EBRecorder.AudioSelection.Vod");
	case AudioSelection::Both:
		return ebTr("EBRecorder.AudioSelection.Both");
	case AudioSelection::Live:
	default:
		return ebTr("EBRecorder.AudioSelection.Live");
	}
}

bool selectedAudioReady(const EbAudioState &audio)
{
	switch (g_audioSelection) {
	case AudioSelection::Vod:
		return audio.vod.active;
	case AudioSelection::Both:
		return audio.live.active && audio.vod.active;
	case AudioSelection::Live:
	default:
		return audio.live.active;
	}
}

QString formatAudioEncoderState(const AudioEncoderInfo &audio)
{
	if (!audio.active)
		return ebTr("EBRecorder.Audio.Inactive");
	if (audio.bitrate > 0)
		return ebTr("EBRecorder.Audio.ActiveWithBitrate").arg(audio.bitrate);
	return ebTr("EBRecorder.Audio.Active");
}

QString formatVodAudioState(const AudioEncoderInfo &vod, const VodTrackConfigState &config)
{
	if (vod.active)
		return formatAudioEncoderState(vod);
	if (config.known && !config.configured)
		return ebTr("EBRecorder.Audio.VodDisabled");
	if (config.known && config.configured)
		return ebTr("EBRecorder.Audio.VodWaiting");
	return formatAudioEncoderState(vod);
}

QString formatAudioStatus(const EbAudioState &audio, const VodTrackConfigState &config)
{
	return ebTr("EBRecorder.Audio.Status")
		.arg(formatAudioEncoderState(audio.live))
		.arg(formatVodAudioState(audio.vod, config))
		.arg(audioSelectionLabel(g_audioSelection));
}

QString obsRecordingDirectory()
{
	config_t *config = obs_frontend_get_profile_config();
	const char *path = nullptr;

	if (config) {
		const char *mode = config_get_string(config, "Output", "Mode");
		if (mode && std::strcmp(mode, "Advanced") == 0) {
			const char *type = config_get_string(config, "AdvOut", "RecType");
			if (type && *type && std::strcmp(type, "Standard") != 0)
				path = config_get_string(config, "AdvOut", "FFFilePath");
			else
				path = config_get_string(config, "AdvOut", "RecFilePath");
		} else {
			path = config_get_string(config, "SimpleOutput", "FilePath");
		}
	}

	QString directory = path && *path ? QString::fromUtf8(path) : QString();
	if (directory.isEmpty())
		directory = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	return QDir::cleanPath(directory);
}

QString makeRecordingPath()
{
	const QString directory = obsRecordingDirectory();
	if (directory.isEmpty())
		return {};

	QDir dir(directory);
	if (!dir.exists() && !QDir().mkpath(directory))
		return {};

	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
	QString candidate = dir.filePath(QStringLiteral("EBRecorder_%1.mkv").arg(stamp));
	int suffix = 2;
	while (QFileInfo::exists(candidate)) {
		candidate = dir.filePath(QStringLiteral("EBRecorder_%1_%2.mkv").arg(stamp).arg(suffix++));
	}
	return QDir::toNativeSeparators(candidate);
}

// OBS's ffmpeg_muxer passes the AAC bitrate to FFmpeg, but Matroska has no
// native TrackEntry bitrate field. After a clean stop, add standard Matroska
// BPS tags for the audio TrackUIDs. The tags are appended without remuxing,
// and a new SeekHead entry is fitted into FFmpeg's existing metadata reserve.
// Encoded media packets and all cluster/cue offsets remain untouched.
struct EbmlElement {
	quint64 id = 0;
	quint64 size = 0;
	quint64 start = 0;
	quint64 payloadStart = 0;
	quint64 end = 0;
	int idLength = 0;
	int sizeLength = 0;
	bool unknownSize = false;
};

int ebmlVintLength(uint8_t first)
{
	uint8_t mask = 0x80;
	for (int length = 1; length <= 8; ++length, mask >>= 1) {
		if (first & mask)
			return length;
	}
	return 0;
}

bool readEbmlElementHeader(const QByteArray &data, quint64 offset, EbmlElement &element)
{
	const quint64 dataSize = static_cast<quint64>(data.size());
	if (offset >= dataSize)
		return false;

	const auto byteAt = [&data](quint64 index) {
		return static_cast<uint8_t>(data.at(static_cast<qsizetype>(index)));
	};

	const int idLength = ebmlVintLength(byteAt(offset));
	if (idLength <= 0 || idLength > 4 || offset + static_cast<quint64>(idLength) > dataSize)
		return false;

	quint64 id = 0;
	for (int i = 0; i < idLength; ++i)
		id = (id << 8) | byteAt(offset + static_cast<quint64>(i));

	const quint64 sizeOffset = offset + static_cast<quint64>(idLength);
	if (sizeOffset >= dataSize)
		return false;
	const int sizeLength = ebmlVintLength(byteAt(sizeOffset));
	if (sizeLength <= 0 || sizeOffset + static_cast<quint64>(sizeLength) > dataSize)
		return false;

	quint64 rawSize = 0;
	for (int i = 0; i < sizeLength; ++i)
		rawSize = (rawSize << 8) | byteAt(sizeOffset + static_cast<quint64>(i));
	const quint64 valueMask = (quint64{1} << (7 * sizeLength)) - 1;
	const quint64 size = rawSize & valueMask;
	const bool unknownSize = size == valueMask;
	const quint64 payloadStart = sizeOffset + static_cast<quint64>(sizeLength);
	if (!unknownSize && size > UINT64_MAX - payloadStart)
		return false;

	element.id = id;
	element.size = size;
	element.start = offset;
	element.payloadStart = payloadStart;
	element.end = unknownSize ? UINT64_MAX : payloadStart + size;
	element.idLength = idLength;
	element.sizeLength = sizeLength;
	element.unknownSize = unknownSize;
	return !unknownSize || element.end >= element.payloadStart;
}

QByteArray encodeEbmlId(quint64 id)
{
	int width = 1;
	while (width < 8 && id >= (quint64{1} << (8 * width)))
		++width;

	QByteArray out(width, '\0');
	for (int i = width - 1; i >= 0; --i) {
		out[i] = static_cast<char>(id & 0xFF);
		id >>= 8;
	}
	return out;
}

QByteArray encodeEbmlSize(quint64 value, int requestedWidth = 0)
{
	int width = requestedWidth;
	if (width == 0) {
		for (width = 1; width <= 8; ++width) {
			const quint64 marker = quint64{1} << (7 * width);
			if (value < marker - 1)
				break;
		}
	}

	if (width < 1 || width > 8)
		return {};
	const quint64 marker = quint64{1} << (7 * width);
	if (value >= marker - 1)
		return {};

	quint64 raw = marker | value;
	QByteArray out(width, '\0');
	for (int i = width - 1; i >= 0; --i) {
		out[i] = static_cast<char>(raw & 0xFF);
		raw >>= 8;
	}
	return out;
}

QByteArray encodeEbmlUInt(quint64 value)
{
	int width = 1;
	while (width < 8 && value >= (quint64{1} << (8 * width)))
		++width;

	QByteArray out(width, '\0');
	for (int i = width - 1; i >= 0; --i) {
		out[i] = static_cast<char>(value & 0xFF);
		value >>= 8;
	}
	return out;
}

QByteArray makeEbmlElement(quint64 id, const QByteArray &payload, int sizeWidth = 0)
{
	const QByteArray size = encodeEbmlSize(static_cast<quint64>(payload.size()), sizeWidth);
	if (size.isEmpty())
		return {};
	return encodeEbmlId(id) + size + payload;
}

QByteArray makeEbmlVoid(quint64 totalBytes)
{
	if (totalBytes == 0)
		return {};

	for (int sizeWidth = 1; sizeWidth <= 8; ++sizeWidth) {
		if (totalBytes < static_cast<quint64>(1 + sizeWidth))
			continue;
		const quint64 payloadSize = totalBytes - static_cast<quint64>(1 + sizeWidth);
		const QByteArray encodedSize = encodeEbmlSize(payloadSize, sizeWidth);
		if (encodedSize.isEmpty())
			continue;
		QByteArray result;
		result.reserve(static_cast<qsizetype>(totalBytes));
		result.append(static_cast<char>(0xEC));
		result.append(encodedSize);
		result.append(QByteArray(static_cast<qsizetype>(payloadSize), '\0'));
		return result;
	}
	return {};
}

bool collectAudioTrackUids(const QByteArray &header, const EbmlElement &tracks,
			   std::vector<QByteArray> &audioTrackUids)
{
	if (tracks.unknownSize || tracks.end > static_cast<quint64>(header.size()))
		return false;

	quint64 cursor = tracks.payloadStart;
	while (cursor < tracks.end) {
		EbmlElement trackEntry;
		if (!readEbmlElementHeader(header, cursor, trackEntry) || trackEntry.unknownSize ||
		    trackEntry.end > tracks.end || trackEntry.end > static_cast<quint64>(header.size()))
			return false;

		if (trackEntry.id == 0xAE) {
			QByteArray uid;
			quint64 trackType = 0;
			quint64 childCursor = trackEntry.payloadStart;
			while (childCursor < trackEntry.end) {
				EbmlElement child;
				if (!readEbmlElementHeader(header, childCursor, child) || child.unknownSize ||
				    child.end > trackEntry.end || child.end > static_cast<quint64>(header.size()))
					return false;

				if (child.id == 0x73C5) {
					uid = header.mid(static_cast<qsizetype>(child.payloadStart),
							 static_cast<qsizetype>(child.size));
				} else if (child.id == 0x83) {
					for (quint64 i = 0; i < child.size; ++i) {
						trackType = (trackType << 8) |
							static_cast<uint8_t>(header.at(static_cast<qsizetype>(child.payloadStart + i)));
					}
				}
				childCursor = child.end;
			}

			if (trackType == 2 && !uid.isEmpty())
				audioTrackUids.push_back(uid);
		}

		cursor = trackEntry.end;
	}
	return true;
}

QByteArray buildBpsTags(const std::vector<QByteArray> &audioTrackUids,
			const std::vector<int64_t> &bitratesBps)
{
	if (audioTrackUids.size() != bitratesBps.size())
		return {};

	QByteArray tagsPayload;
	for (size_t i = 0; i < audioTrackUids.size(); ++i) {
		if (bitratesBps[i] <= 0)
			continue;

		const QByteArray targets = makeEbmlElement(0x63C5, audioTrackUids[i]);
		const QByteArray simpleTagPayload =
			makeEbmlElement(0x45A3, QByteArrayLiteral("BPS")) +
			makeEbmlElement(0x4487, QByteArray::number(bitratesBps[i]));
		const QByteArray tagPayload = makeEbmlElement(0x63C0, targets) +
					      makeEbmlElement(0x67C8, simpleTagPayload);
		tagsPayload += makeEbmlElement(0x7373, tagPayload);
	}

	return tagsPayload.isEmpty() ? QByteArray() : makeEbmlElement(0x1254C367, tagsPayload);
}

bool fitSeekHeadIntoReservedSpan(const QByteArray &seekPayload, quint64 spanBytes, QByteArray &region)
{
	for (int sizeWidth = 1; sizeWidth <= 8; ++sizeWidth) {
		const QByteArray seekHead = makeEbmlElement(0x114D9B74, seekPayload, sizeWidth);
		if (seekHead.isEmpty() || static_cast<quint64>(seekHead.size()) > spanBytes)
			continue;

		const quint64 remaining = spanBytes - static_cast<quint64>(seekHead.size());
		QByteArray padding;
		if (remaining > 0) {
			padding = makeEbmlVoid(remaining);
			if (padding.isEmpty())
				continue;
		}

		region = seekHead + padding;
		return static_cast<quint64>(region.size()) == spanBytes;
	}
	return false;
}

bool writeExact(QFile &file, quint64 offset, const QByteArray &data)
{
	return file.seek(static_cast<qint64>(offset)) && file.write(data) == data.size();
}

bool applyMatroskaAudioBitrateMetadata(const QString &path, const std::vector<int64_t> &bitratesBps,
				       QString &detail)
{
	if (bitratesBps.empty()) {
		detail = QStringLiteral("no audio bitrate values were captured");
		return false;
	}

	QFile file(path);
	if (!file.open(QIODevice::ReadWrite)) {
		detail = QStringLiteral("could not open finalized MKV for metadata update");
		return false;
	}

	const qint64 fileSizeSigned = file.size();
	if (fileSizeSigned <= 0) {
		detail = QStringLiteral("finalized MKV is empty");
		return false;
	}
	const quint64 originalFileSize = static_cast<quint64>(fileSizeSigned);
	const qint64 headerBytes = std::min<qint64>(fileSizeSigned, kMatroskaHeaderScanBytes);
	const QByteArray header = file.read(headerBytes);
	if (header.size() != headerBytes) {
		detail = QStringLiteral("could not read Matroska header");
		return false;
	}

	EbmlElement segment;
	bool segmentFound = false;
	quint64 cursor = 0;
	while (cursor < static_cast<quint64>(header.size())) {
		EbmlElement element;
		if (!readEbmlElementHeader(header, cursor, element))
			break;
		if (element.id == 0x18538067) {
			segment = element;
			segmentFound = true;
			break;
		}
		if (element.unknownSize || element.end > static_cast<quint64>(header.size()))
			break;
		cursor = element.end;
	}
	if (!segmentFound) {
		detail = QStringLiteral("Matroska Segment element was not found");
		return false;
	}
	if (!segment.unknownSize && segment.end != originalFileSize) {
		detail = QStringLiteral("Matroska Segment does not end at EOF; refusing an in-place metadata update");
		return false;
	}

	EbmlElement seekHead;
	EbmlElement seekPadding;
	EbmlElement tracks;
	bool seekFound = false;
	bool seekPaddingFound = false;
	bool tracksFound = false;
	cursor = segment.payloadStart;
	while (cursor < static_cast<quint64>(header.size())) {
		EbmlElement element;
		if (!readEbmlElementHeader(header, cursor, element))
			break;
		if (element.id == 0x1F43B675)
			break;
		if (element.unknownSize || element.end > static_cast<quint64>(header.size()))
			break;

		if (element.id == 0x114D9B74 && !seekFound) {
			seekHead = element;
			seekFound = true;
		} else if (seekFound && !seekPaddingFound && element.start == seekHead.end && element.id == 0xEC) {
			seekPadding = element;
			seekPaddingFound = true;
		}
		if (element.id == 0x1654AE6B && !tracksFound) {
			tracks = element;
			tracksFound = true;
		}
		cursor = element.end;
	}

	if (!seekFound || !seekPaddingFound || !tracksFound) {
		detail = QStringLiteral("required Matroska SeekHead/metadata reserve/Tracks structure was not found");
		return false;
	}

	std::vector<QByteArray> audioTrackUids;
	if (!collectAudioTrackUids(header, tracks, audioTrackUids)) {
		detail = QStringLiteral("could not parse Matroska audio TrackUID values");
		return false;
	}
	if (audioTrackUids.size() != bitratesBps.size()) {
		detail = QStringLiteral("Matroska audio track count (%1) does not match recorded EB audio encoder count (%2)")
				 .arg(static_cast<qulonglong>(audioTrackUids.size()))
				 .arg(static_cast<qulonglong>(bitratesBps.size()));
		return false;
	}

	const QByteArray bpsTags = buildBpsTags(audioTrackUids, bitratesBps);
	if (bpsTags.isEmpty()) {
		detail = QStringLiteral("no positive audio bitrate values were available for BPS tags");
		return false;
	}

	QByteArray seekPayload;
	cursor = seekHead.payloadStart;
	while (cursor < seekHead.end) {
		EbmlElement child;
		if (!readEbmlElementHeader(header, cursor, child) || child.unknownSize || child.end > seekHead.end) {
			detail = QStringLiteral("could not parse Matroska SeekHead");
			return false;
		}
		if (child.id != 0xBF) {
			seekPayload += header.mid(static_cast<qsizetype>(child.start),
						 static_cast<qsizetype>(child.end - child.start));
		}
		cursor = child.end;
	}

	const quint64 newTagsRelativePosition = originalFileSize - segment.payloadStart;
	const QByteArray seekEntryPayload =
		makeEbmlElement(0x53AB, encodeEbmlId(0x1254C367)) +
		makeEbmlElement(0x53AC, encodeEbmlUInt(newTagsRelativePosition));
	seekPayload += makeEbmlElement(0x4DBB, seekEntryPayload);

	const quint64 reservedSpan = seekPadding.end - seekHead.start;
	QByteArray newSeekRegion;
	if (!fitSeekHeadIntoReservedSpan(seekPayload, reservedSpan, newSeekRegion)) {
		detail = QStringLiteral("Matroska SeekHead reserve is too small for the audio bitrate metadata entry");
		return false;
	}

	QByteArray newSegmentSize;
	QByteArray originalSegmentSize;
	if (!segment.unknownSize) {
		if (segment.size > UINT64_MAX - static_cast<quint64>(bpsTags.size())) {
			detail = QStringLiteral("Matroska Segment size overflow");
			return false;
		}
		newSegmentSize = encodeEbmlSize(segment.size + static_cast<quint64>(bpsTags.size()), segment.sizeLength);
		if (newSegmentSize.isEmpty()) {
			detail = QStringLiteral("Matroska Segment size field cannot represent the metadata extension");
			return false;
		}
		originalSegmentSize = header.mid(static_cast<qsizetype>(segment.start + segment.idLength), segment.sizeLength);
	}

	const QByteArray originalSeekRegion =
		header.mid(static_cast<qsizetype>(seekHead.start), static_cast<qsizetype>(reservedSpan));
	auto rollback = [&]() {
		writeExact(file, seekHead.start, originalSeekRegion);
		if (!segment.unknownSize)
			writeExact(file, segment.start + static_cast<quint64>(segment.idLength), originalSegmentSize);
		file.resize(static_cast<qint64>(originalFileSize));
		file.flush();
	};

	if (!writeExact(file, originalFileSize, bpsTags)) {
		file.resize(static_cast<qint64>(originalFileSize));
		detail = QStringLiteral("could not append Matroska BPS tags");
		return false;
	}

	if (!segment.unknownSize) {
		if (!writeExact(file, segment.start + static_cast<quint64>(segment.idLength), newSegmentSize)) {
			rollback();
			detail = QStringLiteral("could not extend Matroska Segment size");
			return false;
		}
	}

	if (!writeExact(file, seekHead.start, newSeekRegion)) {
		rollback();
		detail = QStringLiteral("could not update Matroska SeekHead");
		return false;
	}

	if (!file.flush()) {
		rollback();
		detail = QStringLiteral("could not flush Matroska bitrate metadata");
		return false;
	}

	detail = QStringLiteral("wrote BPS tags for %1 audio track(s)")
			 .arg(static_cast<qulonglong>(bitratesBps.size()));
	return true;
}

bool recordingOutputActive()
{
	return g_recordingOutput && obs_output_active(g_recordingOutput);
}

void releaseInactiveRecordingOutput()
{
	if (!g_recordingOutput || obs_output_active(g_recordingOutput))
		return;

	const QString finalizedPath = g_recordingPath;
	const std::vector<int64_t> finalizedAudioBitrates = g_recordingAudioBitratesBps;
	const bool updateBitrateMetadata = g_audioBitrateMetadataPending && g_cleanStopRequested;

	blog(LOG_INFO, "[EB Recorder] releasing inactive local output");
	obs_output_release(g_recordingOutput);
	g_recordingOutput = nullptr;

	g_recordingAudioBitratesBps.clear();
	g_audioBitrateMetadataPending = false;
	g_cleanStopRequested = false;

	if (updateBitrateMetadata && !finalizedPath.isEmpty()) {
		QString detail;
		if (applyMatroskaAudioBitrateMetadata(finalizedPath, finalizedAudioBitrates, detail)) {
			blog(LOG_INFO, "[EB Recorder] Matroska audio bitrate metadata: %s", detail.toUtf8().constData());
		} else {
			blog(LOG_WARNING, "[EB Recorder] Matroska audio bitrate metadata skipped: %s",
			     detail.toUtf8().constData());
		}
	}

	if (g_recordingUiState == RecordingUiState::Stopping ||
	    g_recordingUiState == RecordingUiState::Recording) {
		g_recordingUiState = RecordingUiState::Stopped;
	}
}

void setRecordingError(const QString &error)
{
	g_recordingError = error;
	g_recordingUiState = RecordingUiState::Error;
	blog(LOG_ERROR, "[EB Recorder] %s", error.toUtf8().constData());
}

bool startLocalRecording(const EncoderInfo &top, QString &error)
{
	releaseInactiveRecordingOutput();
	if (recordingOutputActive()) {
		error = ebTr("EBRecorder.Error.AlreadyActive");
		return false;
	}

	obs_encoder_t *video = obs_get_encoder_by_name(top.name.c_str());
	if (!video || !obs_encoder_active(video)) {
		if (video)
			obs_encoder_release(video);
		error = ebTr("EBRecorder.Error.NoTop");
		return false;
	}

	struct AcquiredAudio {
		obs_encoder_t *encoder = nullptr;
		const char *name = nullptr;
		int64_t bitrateKbps = 0;
	};
	std::vector<AcquiredAudio> audioEncoders;
	audioEncoders.reserve(2);

	auto releaseAcquiredAudio = [&]() {
		for (auto &entry : audioEncoders) {
			if (entry.encoder) {
				obs_encoder_release(entry.encoder);
				entry.encoder = nullptr;
			}
		}
	};

	auto acquireAudio = [&](const char *name) -> bool {
		obs_encoder_t *encoder = obs_get_encoder_by_name(name);
		if (!encoder || !obs_encoder_active(encoder)) {
			if (encoder)
				obs_encoder_release(encoder);
			return false;
		}

		int64_t bitrate = 0;
		obs_data_t *encoderSettings = obs_encoder_get_settings(encoder);
		if (encoderSettings) {
			bitrate = obs_data_get_int(encoderSettings, "bitrate");
			obs_data_release(encoderSettings);
		}
		audioEncoders.push_back({encoder, name, bitrate});
		return true;
	};

	bool audioOk = true;
	switch (g_audioSelection) {
	case AudioSelection::Vod:
		audioOk = acquireAudio(kEbVodAudioEncoderName);
		break;
	case AudioSelection::Both:
		audioOk = acquireAudio(kEbLiveAudioEncoderName) && acquireAudio(kEbVodAudioEncoderName);
		break;
	case AudioSelection::Live:
	default:
		audioOk = acquireAudio(kEbLiveAudioEncoderName);
		break;
	}

	if (!audioOk) {
		releaseAcquiredAudio();
		obs_encoder_release(video);
		error = ebTr("EBRecorder.Error.NoAudio").arg(audioSelectionLabel(g_audioSelection));
		return false;
	}

	const QString path = makeRecordingPath();
	if (path.isEmpty()) {
		releaseAcquiredAudio();
		obs_encoder_release(video);
		error = ebTr("EBRecorder.Error.Path");
		return false;
	}

	blog(LOG_INFO, "[EB Recorder] recording start requested");
	blog(LOG_INFO, "[EB Recorder] TOP encoder acquired: %p name='%s' codec=%s %ux%u bitrate=%lld kbps",
	     static_cast<void *>(video), top.name.c_str(), top.codec.c_str(), top.width, top.height,
	     static_cast<long long>(top.bitrate));
	blog(LOG_INFO, "[EB Recorder] audio selection: %s (%d track(s))",
	     audioSelectionKey(g_audioSelection).toUtf8().constData(), static_cast<int>(audioEncoders.size()));
	for (size_t i = 0; i < audioEncoders.size(); ++i) {
		const auto &entry = audioEncoders[i];
		const char *codec = obs_encoder_get_codec(entry.encoder);
		blog(LOG_INFO, "[EB Recorder] audio encoder %zu acquired: %p name='%s' codec=%s bitrate=%lld kbps", i,
		     static_cast<void *>(entry.encoder), entry.name, codec ? codec : "unknown",
		     static_cast<long long>(entry.bitrateKbps));
	}

	obs_data_t *settings = obs_data_create();
	const QByteArray pathUtf8 = path.toUtf8();
	obs_data_set_string(settings, "path", pathUtf8.constData());
	obs_data_set_string(settings, "muxer_settings", kMuxerSettings);
	obs_output_t *output = obs_output_create(kLocalOutputType, kLocalOutputName, settings, nullptr);
	obs_data_release(settings);

	if (!output) {
		releaseAcquiredAudio();
		obs_encoder_release(video);
		error = ebTr("EBRecorder.Error.OutputCreate");
		return false;
	}

	blog(LOG_INFO, "[EB Recorder] local output created: %p type=%s container=mkv path='%s'",
	     static_cast<void *>(output), kLocalOutputType, pathUtf8.constData());
	blog(LOG_INFO, "[EB Recorder] crash-resilience: Matroska clusters <= 1000 ms (muxer_settings='%s')",
	     kMuxerSettings);

	obs_output_set_video_encoder2(output, video, 0);
	for (size_t i = 0; i < audioEncoders.size(); ++i)
		obs_output_set_audio_encoder(output, audioEncoders[i].encoder, i);

	const bool videoReused = obs_output_get_video_encoder2(output, 0) == video;
	bool audioReused = true;
	blog(LOG_INFO, "[EB Recorder] attached existing TOP video encoder: %s (output=%p encoder=%p)",
	     videoReused ? "YES" : "NO", static_cast<void *>(output), static_cast<void *>(video));
	for (size_t i = 0; i < audioEncoders.size(); ++i) {
		const bool reused = obs_output_get_audio_encoder(output, i) == audioEncoders[i].encoder;
		audioReused = audioReused && reused;
		blog(LOG_INFO, "[EB Recorder] attached existing EB audio encoder %zu: %s (output=%p encoder=%p name='%s')",
		     i, reused ? "YES" : "NO", static_cast<void *>(output), static_cast<void *>(audioEncoders[i].encoder),
		     audioEncoders[i].name);
	}

	std::vector<int64_t> capturedBitratesBps;
	capturedBitratesBps.reserve(audioEncoders.size());
	for (const auto &entry : audioEncoders)
		capturedBitratesBps.push_back(entry.bitrateKbps > 0 ? entry.bitrateKbps * 1000 : 0);

	releaseAcquiredAudio();
	obs_encoder_release(video);

	if (!videoReused || !audioReused) {
		obs_output_release(output);
		error = ebTr("EBRecorder.Error.AttachReuse");
		return false;
	}

	if (!obs_output_start(output)) {
		const char *lastError = obs_output_get_last_error(output);
		const QString detail = lastError && *lastError
					       ? QStringLiteral(": %1").arg(QString::fromUtf8(lastError))
					       : QString();
		error = ebTr("EBRecorder.Error.OutputStart").arg(detail);
		blog(LOG_ERROR, "[EB Recorder] local output start failed%s%s", lastError && *lastError ? ": " : "",
		     lastError && *lastError ? lastError : "");
		obs_output_release(output);
		return false;
	}

	g_recordingOutput = output;
	g_recordingPath = path;
	g_recordingError.clear();
	g_recordingUiState = RecordingUiState::Recording;
	g_recordingAudioBitratesBps = std::move(capturedBitratesBps);
	g_audioBitrateMetadataPending = std::any_of(g_recordingAudioBitratesBps.begin(),
							      g_recordingAudioBitratesBps.end(),
							      [](int64_t value) { return value > 0; });
	g_cleanStopRequested = false;

	blog(LOG_INFO, "[EB Recorder] recording started: container=mkv path='%s'", pathUtf8.constData());
	blog(LOG_INFO, "[EB Recorder] encoder reuse invariant: no encoder was created by EB Recorder");
	return true;
}

void stopLocalRecording(bool force, const char *reason)
{
	if (!g_recordingOutput)
		return;

	if (!obs_output_active(g_recordingOutput)) {
		releaseInactiveRecordingOutput();
		return;
	}

	blog(LOG_INFO, "[EB Recorder] stopping local recording (%s, force=%s)", reason ? reason : "unspecified",
	     force ? "yes" : "no");
	g_recordingUiState = RecordingUiState::Stopping;
	g_cleanStopRequested = !force;

	if (force)
		obs_output_force_stop(g_recordingOutput);
	else
		obs_output_stop(g_recordingOutput);
}

void refreshDialogIfOpen();

void cleanupRecordingOutput()
{
	if (!g_recordingOutput)
		return;

	if (obs_output_active(g_recordingOutput)) {
		blog(LOG_WARNING, "[EB Recorder] local output still active during cleanup; forcing stop");
		g_cleanStopRequested = false;
		obs_output_force_stop(g_recordingOutput);
	}

	obs_output_release(g_recordingOutput);
	g_recordingOutput = nullptr;
	g_recordingAudioBitratesBps.clear();
	g_audioBitrateMetadataPending = false;
	g_cleanStopRequested = false;
	g_recordingUiState = RecordingUiState::Stopped;
}

void initializeSettingsPath()
{
	char *path = obs_module_config_path("settings.ini");
	if (!path) {
		blog(LOG_WARNING, "[EB Recorder] plugin settings path is unavailable; plugin settings will not persist");
		return;
	}

	g_settingsPath = QString::fromUtf8(path);
	bfree(path);

	const QFileInfo info(g_settingsPath);
	if (!QDir().mkpath(info.absolutePath())) {
		blog(LOG_WARNING, "[EB Recorder] could not create settings directory: %s",
		     info.absolutePath().toUtf8().constData());
		g_settingsPath.clear();
		return;
	}

	QSettings settings(g_settingsPath, QSettings::IniFormat);
	g_autoRecordEnabled = settings.value(QStringLiteral("General/AutoRecordWithStream"), false).toBool();
	g_audioSelection = audioSelectionFromKey(
		settings.value(QStringLiteral("General/AudioSelection"), QStringLiteral("live")).toString());
	blog(LOG_INFO, "[EB Recorder] settings loaded: auto_record_with_stream=%s audio_selection=%s path='%s'",
	     g_autoRecordEnabled ? "yes" : "no", audioSelectionKey(g_audioSelection).toUtf8().constData(),
	     g_settingsPath.toUtf8().constData());
}

void savePluginSettings()
{
	if (g_settingsPath.isEmpty())
		return;

	QSettings settings(g_settingsPath, QSettings::IniFormat);
	settings.setValue(QStringLiteral("General/AutoRecordWithStream"), g_autoRecordEnabled);
	settings.setValue(QStringLiteral("General/AudioSelection"), audioSelectionKey(g_audioSelection));
	settings.sync();
	if (settings.status() != QSettings::NoError) {
		blog(LOG_WARNING, "[EB Recorder] failed to save plugin settings to '%s'",
		     g_settingsPath.toUtf8().constData());
	}
}

bool normalizeAudioSelectionForVodAvailability(const EbAudioState &audio, const VodTrackConfigState &vodConfig,
					       const char *reason)
{
	if (g_audioSelection == AudioSelection::Live || recordingOutputActive() ||
	    vodSelectionAllowed(audio, vodConfig)) {
		return false;
	}

	blog(LOG_INFO,
	     "[EB Recorder] audio selection '%s' is unavailable with current OBS VOD settings; falling back to Live (%s)",
	     audioSelectionKey(g_audioSelection).toUtf8().constData(), reason ? reason : "unspecified");
	g_audioSelection = AudioSelection::Live;
	savePluginSettings();
	return true;
}

void stopAutoStartTimer()
{
	if (g_autoStartTimer)
		g_autoStartTimer->stop();
}

void cancelPendingAutoStart(const char *reason)
{
	if (!g_autoStartPending) {
		stopAutoStartTimer();
		return;
	}

	blog(LOG_INFO, "[EB Recorder] automatic recording request cancelled (%s)",
	     reason ? reason : "unspecified");
	g_autoStartPending = false;
	g_autoStartDeadlineMs = 0;
	stopAutoStartTimer();
}

void tryAutomaticRecordingStart()
{
	if (!g_autoStartPending) {
		stopAutoStartTimer();
		return;
	}

	if (!g_autoRecordEnabled) {
		cancelPendingAutoStart("setting disabled");
		return;
	}

	if (!obs_frontend_streaming_active()) {
		cancelPendingAutoStart("stream is no longer active");
		return;
	}

	releaseInactiveRecordingOutput();
	if (recordingOutputActive()) {
		g_autoStartPending = false;
		g_autoStartDeadlineMs = 0;
		stopAutoStartTimer();
		return;
	}

	const auto encoders = getEbEncoders();
	const EncoderInfo *top = findTopEncoder(encoders);
	const EbAudioState audio = getEbAudioState();
	const VodTrackConfigState vodConfig = getObsVodTrackConfigState();
	normalizeAudioSelectionForVodAvailability(audio, vodConfig, "automatic start");

	if (top && selectedAudioReady(audio)) {
		QString error;
		if (startLocalRecording(*top, error)) {
			blog(LOG_INFO, "[EB Recorder] automatic recording started with EB stream");
		} else {
			setRecordingError(error);
			blog(LOG_ERROR, "[EB Recorder] automatic recording start failed");
		}

		g_autoStartPending = false;
		g_autoStartDeadlineMs = 0;
		stopAutoStartTimer();
		return;
	}

	if (QDateTime::currentMSecsSinceEpoch() >= g_autoStartDeadlineMs) {
		blog(LOG_INFO,
		     "[EB Recorder] automatic recording skipped: no active EB TOP video/audio encoders appeared within %d ms",
		     kAutoStartTimeoutMs);
		g_autoStartPending = false;
		g_autoStartDeadlineMs = 0;
		stopAutoStartTimer();
	}
}

void ensureAutoStartTimer()
{
	if (g_autoStartTimer)
		return;

	g_autoStartTimer = new QTimer();
	g_autoStartTimer->setTimerType(Qt::CoarseTimer);
	g_autoStartTimer->setInterval(kAutoStartRetryMs);
	QObject::connect(g_autoStartTimer, &QTimer::timeout, []() {
		const bool wasPending = g_autoStartPending;
		const bool wasRecording = recordingOutputActive();
		tryAutomaticRecordingStart();

		// Do not repaint the Qt dialog on every retry tick. Refresh it only when
		// the pending/recording state actually changed. This keeps the plugin
		// compositor-quiet on systems where windowed G-SYNC/VRR is enabled.
		if (wasPending != g_autoStartPending || wasRecording != recordingOutputActive())
			refreshDialogIfOpen();
	});
}

void armAutomaticRecording(const char *reason)
{
	if (!g_autoRecordEnabled || recordingOutputActive())
		return;

	ensureAutoStartTimer();
	g_autoStartPending = true;
	g_autoStartDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kAutoStartTimeoutMs;
	blog(LOG_INFO, "[EB Recorder] automatic recording armed (%s); waiting for active EB encoders",
	     reason ? reason : "unspecified");

	tryAutomaticRecordingStart();
	if (g_autoStartPending && g_autoStartTimer && !g_autoStartTimer->isActive())
		g_autoStartTimer->start();
}

void setAutoRecordEnabled(bool enabled)
{
	if (g_autoRecordEnabled == enabled)
		return;

	g_autoRecordEnabled = enabled;
	savePluginSettings();
	blog(LOG_INFO, "[EB Recorder] automatic recording with EB stream: %s", enabled ? "enabled" : "disabled");

	if (!enabled) {
		cancelPendingAutoStart("setting disabled by user");
		return;
	}

	if (obs_frontend_streaming_active() && !recordingOutputActive())
		armAutomaticRecording("setting enabled during active stream");
}

void setAudioSelection(AudioSelection selection)
{
	if (selection != AudioSelection::Live) {
		const EbAudioState audio = getEbAudioState();
		const VodTrackConfigState vodConfig = getObsVodTrackConfigState();
		if (!vodSelectionAllowed(audio, vodConfig)) {
			blog(LOG_INFO, "[EB Recorder] ignored unavailable VOD audio selection; using Live");
			selection = AudioSelection::Live;
		}
	}

	if (g_audioSelection == selection)
		return;

	g_audioSelection = selection;
	savePluginSettings();
	blog(LOG_INFO, "[EB Recorder] audio selection changed: %s",
	     audioSelectionKey(g_audioSelection).toUtf8().constData());

	if (g_autoRecordEnabled && obs_frontend_streaming_active() && !recordingOutputActive())
		armAutomaticRecording("audio selection changed during active stream");
}

bool startRecordingFromCurrentEb(QString &error, const char *reason)
{
	releaseInactiveRecordingOutput();
	if (recordingOutputActive()) {
		error = ebTr("EBRecorder.Error.AlreadyActive");
		return false;
	}

	const EbAudioState audio = getEbAudioState();
	const VodTrackConfigState vodConfig = getObsVodTrackConfigState();
	normalizeAudioSelectionForVodAvailability(audio, vodConfig, reason);

	const auto encoders = getEbEncoders();
	const EncoderInfo *top = findTopEncoder(encoders);
	if (!top) {
		error = ebTr("EBRecorder.Error.NoTop");
		return false;
	}
	if (!selectedAudioReady(audio)) {
		error = ebTr("EBRecorder.Error.NoAudio").arg(audioSelectionLabel(g_audioSelection));
		return false;
	}

	blog(LOG_INFO, "[EB Recorder] recording start requested (%s)", reason ? reason : "unspecified");
	return startLocalRecording(*top, error);
}

obs_data_array_t *loadFrontendHotkeyBindings(config_t *config, const char *name)
{
	obs_data_array_t *bindings = obs_data_array_create();
	if (!config || !name)
		return bindings;

	const char *json = config_get_string(config, "Hotkeys", name);
	if (!json || !*json)
		return bindings;

	obs_data_t *data = obs_data_create_from_json(json);
	if (!data)
		return bindings;

	obs_data_array_t *saved = obs_data_get_array(data, "bindings");
	if (saved) {
		obs_data_array_release(bindings);
		bindings = saved;
	}
	obs_data_release(data);
	return bindings;
}

void loadRecordingHotkeyBindings()
{
	if (g_recordingHotkeys == OBS_INVALID_HOTKEY_PAIR_ID)
		return;

	config_t *config = obs_frontend_get_profile_config();
	obs_data_array_t *startBindings = loadFrontendHotkeyBindings(config, kStartHotkeyName);
	obs_data_array_t *stopBindings = loadFrontendHotkeyBindings(config, kStopHotkeyName);
	obs_hotkey_pair_load(g_recordingHotkeys, startBindings, stopBindings);
	obs_data_array_release(startBindings);
	obs_data_array_release(stopBindings);
	blog(LOG_INFO, "[EB Recorder] recording hotkey bindings loaded from current OBS profile");
}

bool startRecordingHotkey(void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || recordingOutputActive())
		return false;

	QString error;
	if (!startRecordingFromCurrentEb(error, "hotkey")) {
		blog(LOG_INFO, "[EB Recorder] start hotkey ignored: %s", error.toUtf8().constData());
		refreshDialogIfOpen();
		return false;
	}

	cancelPendingAutoStart("recording started by hotkey");
	blog(LOG_INFO, "[EB Recorder] recording started by hotkey");
	refreshDialogIfOpen();
	return true;
}

bool stopRecordingHotkey(void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !recordingOutputActive())
		return false;

	cancelPendingAutoStart("recording stopped by hotkey");
	stopLocalRecording(false, "hotkey");
	blog(LOG_INFO, "[EB Recorder] recording stop requested by hotkey");
	refreshDialogIfOpen();
	return true;
}

void registerRecordingHotkeys()
{
	if (g_recordingHotkeys != OBS_INVALID_HOTKEY_PAIR_ID)
		return;

	const QByteArray startDescription = ebTr("EBRecorder.Hotkey.Start").toUtf8();
	const QByteArray stopDescription = ebTr("EBRecorder.Hotkey.Stop").toUtf8();
	g_recordingHotkeys = obs_hotkey_pair_register_frontend(
		kStartHotkeyName, startDescription.constData(), kStopHotkeyName, stopDescription.constData(),
		startRecordingHotkey, stopRecordingHotkey, nullptr, nullptr);

	if (g_recordingHotkeys == OBS_INVALID_HOTKEY_PAIR_ID) {
		blog(LOG_WARNING, "[EB Recorder] failed to register recording hotkeys");
		return;
	}

	loadRecordingHotkeyBindings();
	blog(LOG_INFO, "[EB Recorder] recording hotkeys registered");
}

void unregisterRecordingHotkeys()
{
	if (g_recordingHotkeys == OBS_INVALID_HOTKEY_PAIR_ID)
		return;

	obs_hotkey_pair_unregister(g_recordingHotkeys);
	g_recordingHotkeys = OBS_INVALID_HOTKEY_PAIR_ID;
	blog(LOG_INFO, "[EB Recorder] recording hotkeys unregistered");
}

class EBRecorderDialog final : public QDialog {
public:
	explicit EBRecorderDialog(QWidget *parent) : QDialog(parent)
	{
		setAttribute(Qt::WA_DeleteOnClose, true);
		setWindowTitle(ebTr("EBRecorder.Title"));
		setMinimumSize(820, 500);
		resize(940, 560);

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(14, 14, 14, 14);
		root->setSpacing(10);

		auto *intro = new QLabel(ebTr("EBRecorder.Intro"), this);
		intro->setWordWrap(true);
		root->addWidget(intro);

		statusLabel_ = new QLabel(this);
		statusLabel_->setWordWrap(true);
		root->addWidget(statusLabel_);

		table_ = new QTableWidget(this);
		table_->setColumnCount(7);
		table_->setHorizontalHeaderLabels({ebTr("EBRecorder.Col.Index"), ebTr("EBRecorder.Col.Codec"),
						  ebTr("EBRecorder.Col.Resolution"), ebTr("EBRecorder.Col.Bitrate"),
						  ebTr("EBRecorder.Col.Active"), ebTr("EBRecorder.Col.Role"),
						  ebTr("EBRecorder.Col.Name")});
		table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table_->setSelectionBehavior(QAbstractItemView::SelectRows);
		table_->setSelectionMode(QAbstractItemView::SingleSelection);
		table_->verticalHeader()->setVisible(false);
		table_->horizontalHeader()->setStretchLastSection(true);
		for (int column = 0; column < 6; ++column)
			table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
		root->addWidget(table_, 1);

		selectedLabel_ = new QLabel(this);
		selectedLabel_->setWordWrap(true);
		root->addWidget(selectedLabel_);

		audioLabel_ = new QLabel(this);
		audioLabel_->setWordWrap(true);
		root->addWidget(audioLabel_);

		recordingLabel_ = new QLabel(this);
		recordingLabel_->setWordWrap(true);
		root->addWidget(recordingLabel_);

		auto *settingsBox = new QGroupBox(ebTr("EBRecorder.Settings"), this);
		auto *settingsLayout = new QVBoxLayout(settingsBox);
		autoRecordCheckBox_ = new QCheckBox(ebTr("EBRecorder.AutoRecord"), settingsBox);
		autoRecordCheckBox_->setChecked(g_autoRecordEnabled);
		autoRecordCheckBox_->setToolTip(ebTr("EBRecorder.AutoRecord.Tooltip"));
		settingsLayout->addWidget(autoRecordCheckBox_);

		auto *audioSelectionRow = new QHBoxLayout;
		auto *audioSelectionLabelWidget = new QLabel(ebTr("EBRecorder.AudioSelection"), settingsBox);
		audioSelectionCombo_ = new QComboBox(settingsBox);
		audioSelectionCombo_->addItem(ebTr("EBRecorder.AudioSelection.Live"));
		audioSelectionCombo_->addItem(ebTr("EBRecorder.AudioSelection.Vod"));
		audioSelectionCombo_->addItem(ebTr("EBRecorder.AudioSelection.Both"));
		audioSelectionCombo_->setCurrentIndex(static_cast<int>(g_audioSelection));
		audioSelectionCombo_->setToolTip(ebTr("EBRecorder.AudioSelection.Tooltip"));
		audioSelectionRow->addWidget(audioSelectionLabelWidget);
		audioSelectionRow->addWidget(audioSelectionCombo_, 1);
		settingsLayout->addLayout(audioSelectionRow);
		root->addWidget(settingsBox);

		auto *buttons = new QHBoxLayout;
		buttons->addStretch(1);
		auto *refreshButton = new QPushButton(ebTr("EBRecorder.Refresh"), this);
		startButton_ = new QPushButton(ebTr("EBRecorder.StartRecording"), this);
		stopButton_ = new QPushButton(ebTr("EBRecorder.StopRecording"), this);
		auto *closeButton = new QPushButton(ebTr("EBRecorder.Close"), this);
		buttons->addWidget(refreshButton);
		buttons->addWidget(startButton_);
		buttons->addWidget(stopButton_);
		buttons->addWidget(closeButton);
		root->addLayout(buttons);

		connect(refreshButton, &QPushButton::clicked, this, [this]() { refreshEncoders(true); });
		connect(autoRecordCheckBox_, &QCheckBox::toggled, this, [this](bool checked) {
			setAutoRecordEnabled(checked);
			refreshEncoders(true);
		});
		connect(audioSelectionCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
			if (index >= 0 && index <= static_cast<int>(AudioSelection::Both))
				setAudioSelection(static_cast<AudioSelection>(index));
			refreshEncoders(true);
		});
		connect(startButton_, &QPushButton::clicked, this, [this]() { startRecording(); });
		connect(stopButton_, &QPushButton::clicked, this, [this]() {
			stopLocalRecording(false, "user request");
			refreshEncoders(true);
		});
		connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

		timer_ = new QTimer(this);
		timer_->setTimerType(Qt::CoarseTimer);
		timer_->setInterval(kDialogRefreshMs);
		connect(timer_, &QTimer::timeout, this, [this]() { refreshEncoders(false); });
		timer_->start();

		refreshEncoders(true);
		blog(LOG_INFO, "[EB Recorder] dialog created");
	}

	~EBRecorderDialog() override
	{
		if (timer_)
			timer_->stop();
		blog(LOG_INFO, "[EB Recorder] dialog destroyed");
	}

	void refreshNow() { refreshEncoders(true); }

private:
	void updateAudioSelectionChoices(bool vodSelectable)
	{
		const int state = vodSelectable ? 1 : 0;
		if (lastVodSelectable_ == state)
			return;
		lastVodSelectable_ = state;

		auto *model = qobject_cast<QStandardItemModel *>(audioSelectionCombo_->model());
		if (model) {
			if (auto *vodItem = model->item(static_cast<int>(AudioSelection::Vod)))
				vodItem->setEnabled(vodSelectable);
			if (auto *bothItem = model->item(static_cast<int>(AudioSelection::Both)))
				bothItem->setEnabled(vodSelectable);
		}

		const QString baseTooltip = ebTr("EBRecorder.AudioSelection.Tooltip");
		audioSelectionCombo_->setToolTip(vodSelectable
						 ? baseTooltip
						 : baseTooltip + QStringLiteral("\n\n") +
							   ebTr("EBRecorder.AudioSelection.VodUnavailable"));
	}

	void startRecording()
	{
		QString error;
		if (!startRecordingFromCurrentEb(error, "dialog button"))
			setRecordingError(error);
		else
			cancelPendingAutoStart("recording started from dialog");
		refreshEncoders(true);
	}

	void updateRecordingUi(bool topAvailable, bool audioAvailable)
	{
		const bool active = recordingOutputActive();

		if (active && g_recordingUiState != RecordingUiState::Stopping)
			g_recordingUiState = RecordingUiState::Recording;

		audioSelectionCombo_->setEnabled(!active);

		if (!active && g_autoStartPending) {
			recordingLabel_->setText(ebTr("EBRecorder.Recording.AutoPending"));
			startButton_->setEnabled(topAvailable && audioAvailable);
			stopButton_->setEnabled(false);
			return;
		}

		switch (g_recordingUiState) {
		case RecordingUiState::Recording:
			recordingLabel_->setText(ebTr("EBRecorder.Recording.Active").arg(g_recordingPath));
			break;
		case RecordingUiState::Stopping:
			recordingLabel_->setText(ebTr("EBRecorder.Recording.Stopping").arg(g_recordingPath));
			break;
		case RecordingUiState::Stopped:
			recordingLabel_->setText(g_recordingPath.isEmpty()
						 ? ebTr("EBRecorder.Recording.Idle")
						 : ebTr("EBRecorder.Recording.Stopped").arg(g_recordingPath));
			break;
		case RecordingUiState::Error:
			recordingLabel_->setText(ebTr("EBRecorder.Recording.Error").arg(g_recordingError));
			break;
		case RecordingUiState::Idle:
		default:
			recordingLabel_->setText(ebTr("EBRecorder.Recording.Idle"));
			break;
		}

		startButton_->setEnabled(!active && topAvailable && audioAvailable);
		stopButton_->setEnabled(active);
	}

	void refreshEncoders(bool forceLog)
	{
		releaseInactiveRecordingOutput();

		const auto encoders = getEbEncoders();
		const EncoderInfo *top = findTopEncoder(encoders);
		const EbAudioState audio = getEbAudioState();
		const VodTrackConfigState vodConfig = getObsVodTrackConfigState();
		const bool vodSelectable = vodSelectionAllowed(audio, vodConfig);
		normalizeAudioSelectionForVodAvailability(audio, vodConfig, "dialog refresh");
		if (audioSelectionCombo_->currentIndex() != static_cast<int>(g_audioSelection)) {
			QSignalBlocker blocker(audioSelectionCombo_);
			audioSelectionCombo_->setCurrentIndex(static_cast<int>(g_audioSelection));
		}
		updateAudioSelectionChoices(vodSelectable);
		const bool audioAvailable = selectedAudioReady(audio);
		const int activeCount = static_cast<int>(std::count_if(encoders.begin(), encoders.end(),
							   [](const EncoderInfo &e) { return e.active; }));

		const QString encoderUiSignature = makeSignature(encoders, top) +
			QStringLiteral("|live=%1:%2|vod=%3:%4|vod_cfg_known=%5|vod_cfg=%6|vod_tracks=%7:%8|audio_selection=%9")
				.arg(audio.live.active ? 1 : 0)
				.arg(audio.live.bitrate)
				.arg(audio.vod.active ? 1 : 0)
				.arg(audio.vod.bitrate)
				.arg(vodConfig.known ? 1 : 0)
				.arg(vodConfig.configured ? 1 : 0)
				.arg(vodConfig.liveTrack)
				.arg(vodConfig.vodTrack)
				.arg(audioSelectionKey(g_audioSelection));
		if (encoderUiSignature != lastEncoderUiSignature_) {
			lastEncoderUiSignature_ = encoderUiSignature;

			// Rebuild the small diagnostics table only when encoder state actually
			// changes. Previously this allocated/repainted every 500 ms even when
			// nothing changed, which was unnecessary compositor activity.
			table_->setUpdatesEnabled(false);
			table_->setRowCount(static_cast<int>(encoders.size()));
			for (int row = 0; row < static_cast<int>(encoders.size()); ++row) {
				const auto &encoder = encoders[static_cast<size_t>(row)];
				const bool isTop = top && encoder.index == top->index;

				const QString bitrate = encoder.bitrate > 0
								? QStringLiteral("%1 kbps").arg(encoder.bitrate)
								: QStringLiteral("—");
				const QString resolution = encoder.width && encoder.height
								   ? QStringLiteral("%1×%2").arg(encoder.width).arg(encoder.height)
								   : QStringLiteral("—");

				const QString values[] = {
					QString::number(encoder.index),
					codecLabel(encoder.codec),
					resolution,
					bitrate,
					encoder.active ? ebTr("EBRecorder.Yes") : ebTr("EBRecorder.No"),
					isTop ? ebTr("EBRecorder.Top") : QStringLiteral("—"),
					QString::fromStdString(encoder.name),
				};

				for (int column = 0; column < 7; ++column) {
					auto *item = new QTableWidgetItem(values[column]);
					if (isTop) {
						QFont font = item->font();
						font.setBold(true);
						item->setFont(font);
					}
					table_->setItem(row, column, item);
				}
			}
			table_->setUpdatesEnabled(true);

			if (!top) {
				statusLabel_->setText(ebTr("EBRecorder.Status.None"));
				selectedLabel_->setText(ebTr("EBRecorder.Selected.None"));
			} else {
				statusLabel_->setText(ebTr("EBRecorder.Status.Found").arg(activeCount));
				selectedLabel_->setText(
					ebTr("EBRecorder.Selected.Found")
						.arg(top->index)
						.arg(codecLabel(top->codec))
						.arg(top->width)
						.arg(top->height)
						.arg(top->bitrate));
			}

			audioLabel_->setText(formatAudioStatus(audio, vodConfig));
		}

		const QString recordingUiSignature =
			QStringLiteral("state=%1|rec=%2|auto=%3|pending=%4|top=%5|audio=%6|path=%7|error=%8")
				.arg(static_cast<int>(g_recordingUiState))
				.arg(recordingOutputActive() ? 1 : 0)
				.arg(g_autoRecordEnabled ? 1 : 0)
				.arg(g_autoStartPending ? 1 : 0)
				.arg(top ? 1 : 0)
				.arg(audioAvailable ? 1 : 0)
				.arg(g_recordingPath)
				.arg(g_recordingError);
		if (recordingUiSignature != lastRecordingUiSignature_) {
			lastRecordingUiSignature_ = recordingUiSignature;
			updateRecordingUi(top != nullptr, audioAvailable);
		}

		const QString logSignature = encoderUiSignature + QLatin1Char('|') + recordingUiSignature;
		if (forceLog || logSignature != lastLogSignature_) {
			lastLogSignature_ = logSignature;
			blog(LOG_INFO,
			     "[EB Recorder] EB encoder scan: %d found, %d active, live_audio=%s/%lldkbps, vod_audio=%s/%lldkbps, vod_config=%s, audio_selection=%s, local_recording=%s, auto_record=%s, auto_pending=%s",
			     static_cast<int>(encoders.size()), activeCount, audio.live.active ? "active" : "inactive",
			     static_cast<long long>(audio.live.bitrate), audio.vod.active ? "active" : "inactive",
			     static_cast<long long>(audio.vod.bitrate), vodConfig.configured ? "enabled" : "disabled",
			     audioSelectionKey(g_audioSelection).toUtf8().constData(), recordingOutputActive() ? "active" : "inactive",
			     g_autoRecordEnabled ? "enabled" : "disabled", g_autoStartPending ? "yes" : "no");
			for (const auto &encoder : encoders) {
				blog(LOG_INFO,
				     "[EB Recorder] encoder %d: %s %ux%u, bitrate=%lld kbps, active=%s%s",
				     encoder.index, encoder.codec.c_str(), encoder.width, encoder.height,
				     static_cast<long long>(encoder.bitrate), encoder.active ? "yes" : "no",
				     top && encoder.index == top->index ? " [TOP]" : "");
			}
		}
	}

	QLabel *statusLabel_ = nullptr;
	QTableWidget *table_ = nullptr;
	QLabel *selectedLabel_ = nullptr;
	QLabel *audioLabel_ = nullptr;
	QLabel *recordingLabel_ = nullptr;
	QCheckBox *autoRecordCheckBox_ = nullptr;
	QComboBox *audioSelectionCombo_ = nullptr;
	QPushButton *startButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;
	QTimer *timer_ = nullptr;
	QString lastEncoderUiSignature_;
	QString lastRecordingUiSignature_;
	QString lastLogSignature_;
	int lastVodSelectable_ = -1;
};

QPointer<EBRecorderDialog> g_dialog;
std::atomic<uint32_t> g_loadRefs{0};

bool releaseLoadReference(uint32_t &remaining)
{
	uint32_t current = g_loadRefs.load(std::memory_order_acquire);
	while (current != 0) {
		if (g_loadRefs.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel,
		                                     std::memory_order_acquire)) {
			remaining = current - 1;
			return true;
		}
	}

	remaining = 0;
	return false;
}

void refreshDialogIfOpen()
{
	if (!g_dialog.isNull())
		g_dialog->refreshNow();
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		cancelPendingAutoStart("OBS streaming stopping");
		if (recordingOutputActive()) {
			blog(LOG_INFO, "[EB Recorder] streaming is stopping; stopping local recording before EB teardown");
			stopLocalRecording(false, "OBS streaming stopping");
		}
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		cancelPendingAutoStart("OBS streaming stopped");
		if (recordingOutputActive()) {
			blog(LOG_WARNING, "[EB Recorder] local output still active after stream stopped; forcing stop");
			stopLocalRecording(true, "OBS streaming stopped fallback");
		}
		releaseInactiveRecordingOutput();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		g_obsExiting = true;
		cancelPendingAutoStart("OBS exit");
		if (recordingOutputActive())
			stopLocalRecording(true, "OBS exit");
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (g_autoRecordEnabled)
			armAutomaticRecording("OBS streaming started");
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		loadRecordingHotkeyBindings();
		break;
	default:
		return;
	}

	refreshDialogIfOpen();
}

void openDialog(void *)
{
	if (g_dialog.isNull()) {
		auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
		g_dialog = new EBRecorderDialog(parent);
	}

	auto *dialog = g_dialog.data();
	if (!dialog)
		return;

	dialog->refreshNow();
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}
} // namespace

const char *obs_module_description(void)
{
	return "EB Recorder: record the highest-resolution Twitch Enhanced Broadcasting rendition by reusing the existing EB encoders.";
}

bool obs_module_load(void)
{
	const uint32_t loadNumber = g_loadRefs.fetch_add(1, std::memory_order_acq_rel) + 1;
	auto *module = obs_current_module();
	blog(LOG_INFO, "[EB Recorder] obs_module_load #%u (module=%p)", loadNumber,
	     static_cast<void *>(module));

	if (loadNumber > 1) {
		blog(LOG_WARNING,
		     "[EB Recorder] duplicate module initialization ignored (active load refs=%u)",
		     loadNumber);
		return true;
	}

	logLocaleDiagnostics();
	initializeSettingsPath();
	g_obsExiting = false;
	registerRecordingHotkeys();
	obs_frontend_add_tools_menu_item("EB Recorder", openDialog, nullptr);
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	g_frontendCallbackRegistered = true;
	blog(LOG_INFO, "[EB Recorder] frontend event callback registered");
	blog(LOG_INFO, "[EB Recorder] loaded (version %s)", kVersion);
	return true;
}

void obs_module_unload(void)
{
	auto *module = obs_current_module();
	uint32_t remaining = 0;
	if (!releaseLoadReference(remaining)) {
		blog(LOG_WARNING, "[EB Recorder] obs_module_unload with no matching load (module=%p)",
		     static_cast<void *>(module));
		return;
	}

	blog(LOG_INFO, "[EB Recorder] obs_module_unload (module=%p, remaining load refs=%u)",
	     static_cast<void *>(module), remaining);

	if (remaining > 0) {
		blog(LOG_INFO, "[EB Recorder] unload deferred; another logical module reference is still active");
		return;
	}

	blog(LOG_INFO, "[EB Recorder] unload begin (dialog=%s, recording=%s)",
	     g_dialog.isNull() ? "null" : "alive", recordingOutputActive() ? "active" : "inactive");

	cancelPendingAutoStart("module unload");
	unregisterRecordingHotkeys();
	if (g_autoStartTimer) {
		delete g_autoStartTimer;
		g_autoStartTimer = nullptr;
	}

	if (g_frontendCallbackRegistered) {
		// During normal OBS shutdown the frontend callback registry is already
		// being torn down before module unload. Avoid asking OBS to remove a
		// callback from an already-cleared registry (which produces a warning).
		if (!g_obsExiting) {
			obs_frontend_remove_event_callback(frontendEvent, nullptr);
			blog(LOG_INFO, "[EB Recorder] frontend event callback removed");
		} else {
			blog(LOG_INFO, "[EB Recorder] frontend event callback removal skipped during OBS shutdown");
		}
		g_frontendCallbackRegistered = false;
	}

	cleanupRecordingOutput();

	if (!g_dialog.isNull()) {
		auto *dialog = g_dialog.data();
		g_dialog.clear();
		delete dialog;
	}

	blog(LOG_INFO, "[EB Recorder] unloaded");
}
