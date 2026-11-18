// Compile the real plugin implementation into a headless regression test.
// A libobs output with no media flags exercises stop delivery without a GPU,
// Twitch connection, frontend window, or installed OBS plugin.
#include "../src/plugin-main.cpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
int destroyedOutputs = 0;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

void processUntilReleased()
{
	QElapsedTimer deadline;
	deadline.start();
	while (g_recordingOutput && deadline.elapsed() < 5000) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	require(!g_recordingOutput, "output did not finish without a dialog");
	require(!g_recordingStopTimer, "stop timer survived finalization");
}

void prepareOutput()
{
	require(!g_recordingOutput, "previous output was not released");
	g_recordingOutput = obs_output_create("eb-recorder-test-output", "test", nullptr, nullptr);
	require(g_recordingOutput != nullptr, "could not create test output");
	g_recordingUiState = RecordingUiState::Recording;
	g_recordingError.clear();
	g_recordingPath.clear();
	g_recordingAudioBitratesBps.clear();
	g_audioBitrateMetadataPending = false;
	g_cleanStopRequested = true;
	connectRecordingOutputEvents(g_recordingOutput);
	require(obs_output_begin_data_capture(g_recordingOutput, 0), "could not activate test output");
}

void emitStopFromWorker(int code, const char *error)
{
	obs_output_t *output = g_recordingOutput;
	std::thread worker([output, code, error]() {
		calldata_t data;
		calldata_init(&data);
		calldata_set_ptr(&data, "output", output);
		calldata_set_int(&data, "code", code);
		calldata_set_string(&data, "last_error", error);
		signal_handler_signal(obs_output_get_signal_handler(output), "stop", &data);
		calldata_free(&data);
	});
	worker.join();
}

QByteArray metadataFixture()
{
	const QByteArray track = makeEbmlElement(0xAE,
		makeEbmlElement(0x73C5, encodeEbmlUInt(42)) + makeEbmlElement(0x83, encodeEbmlUInt(2)));
	const QByteArray payload = makeEbmlElement(0x114D9B74, QByteArray()) + makeEbmlVoid(256) +
		makeEbmlElement(0x1654AE6B, track);
	return makeEbmlElement(0x18538067, payload, 8);
}

void testStop(bool fail)
{
	prepareOutput();
	QTemporaryDir directory;
	require(directory.isValid(), "could not create metadata fixture directory");
	g_recordingPath = directory.filePath("recording.mkv");
	const QByteArray original = metadataFixture();
	QFile fixture(g_recordingPath);
	require(fixture.open(QIODevice::WriteOnly), "could not open metadata fixture");
	require(fixture.write(original) == original.size(), "could not write metadata fixture");
	fixture.close();
	g_recordingAudioBitratesBps = {160000};
	g_audioBitrateMetadataPending = true;
	const int before = destroyedOutputs;

	emitStopFromWorker(fail ? OBS_OUTPUT_NO_SPACE : OBS_OUTPUT_SUCCESS, fail ? "disk full" : "");
	require(!g_recordingStopReceived, "worker mutated recording state directly");
	QCoreApplication::processEvents();
	require(g_recordingStopReceived, "stop event was not delivered to Qt");
	require(g_recordingStopTimer->isActive(), "teardown retry timer was not started");
	require(destroyedOutputs == before, "output was released while still active");
	if (fail) {
		require(g_recordingUiState == RecordingUiState::Error, "runtime failure was not reported");
		require(g_recordingError.contains("disk full"), "muxer error text was lost");
		require(!g_cleanStopRequested, "failed output remained eligible for metadata writes");
	}

	obs_output_end_data_capture(g_recordingOutput);
	processUntilReleased();
	require(destroyedOutputs == before + 1, "output was not released exactly once");
	require(fixture.open(QIODevice::ReadOnly), "could not reopen metadata fixture");
	const QByteArray finalized = fixture.readAll();
	if (fail) {
		require(finalized == original, "failed recording metadata was modified");
		require(g_recordingUiState == RecordingUiState::Error, "finalization erased the error state");
	} else {
		require(finalized.size() > original.size() && finalized.contains("160000"),
			"successful closed-dialog stop did not write BPS metadata");
		require(g_recordingUiState == RecordingUiState::Stopped, "clean stop has wrong UI state");
	}
}

void testCancelledQueuedCallback()
{
	prepareOutput();
	emitStopFromWorker(OBS_OUTPUT_ERROR, "obsolete failure");
	cleanupRecordingOutput();
	prepareOutput();
	QCoreApplication::processEvents();
	require(!g_recordingStopReceived && g_recordingError.isEmpty(),
		"queued callback from retired output affected the new recording");
	cleanupRecordingOutput();
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (!obs_startup("en-US", nullptr, nullptr))
		return 1;
	obs_output_info info = {};
	info.id = "eb-recorder-test-output";
	info.flags = OBS_OUTPUT_ENCODED;
	info.get_name = [](void *) { return "EB Recorder regression output"; };
	info.create = [](obs_data_t *, obs_output_t *output) { return static_cast<void *>(output); };
	info.destroy = [](void *) { ++destroyedOutputs; };
	info.start = [](void *) { return true; };
	info.stop = [](void *output, uint64_t) { obs_output_end_data_capture(static_cast<obs_output_t *>(output)); };
	info.encoded_packet = [](void *, encoder_packet *) {};
	obs_register_output(&info);
	g_recordingEventOwner = new QObject();
	int result = 0;
	try {
		testStop(false);
		testStop(true);
		testCancelledQueuedCallback();
		std::puts("PASS: clean stop, runtime error, delayed deactivation, metadata, retired callbacks");
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		result = 1;
	}
	cleanupRecordingOutput();
	delete g_recordingEventOwner;
	g_recordingEventOwner = nullptr;
	obs_shutdown();
	return result;
}
