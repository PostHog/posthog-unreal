"""Verify automatic owning-viewport GPU readback, resize and stop through the maintained mock."""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import time
import uuid

from PIL import Image
from native_integration import MockServerTestCase

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "Tests/Unreal/PostHogTestHost.uproject"


def renderer_arguments(system, renderer):
    if renderer == "native":
        return []
    if system == "Windows":
        return ["-dx12", "-warp", "-AllowSoftwareRendering"]
    if system == "Linux":
        return ["-vulkan", "-AllowCPUDevices"]
    raise ValueError("Software rendering smoke supports Windows and Linux; use --renderer native on Mac")


def check_software_adapter(properties, system):
    name = properties.get("rhi_adapter", "").lower()
    if system == "Windows" and properties.get("rhi_vendor_id") == 0x1414:
        return
    if system == "Linux" and any(part in name for part in ("llvmpipe", "lavapipe")):
        return
    raise AssertionError(f"Expected a software adapter, got {properties}")


def decode_pattern(envelope, expected_aspect=None):
    """Return a matching JPEG or None for startup/transition frames."""
    snapshots = envelope["properties"]["$snapshot_data"]
    full = next(s for s in snapshots if s["type"] == 2)
    wireframe = full["data"]["wireframes"][0]
    uri = wireframe["base64"]
    if not uri.startswith("data:image/jpeg;base64,"):
        raise AssertionError("Expected a Rust-encoded JPEG")
    jpeg = base64.b64decode(uri.split(",", 1)[1], validate=True)
    with Image.open(io.BytesIO(jpeg)) as image:
        if image.format != "JPEG":
            raise AssertionError("Image is not JPEG")
        image = image.convert("RGB")
        width, height = image.size
        if not (64 <= max(width, height) <= 320 and min(width, height) > 0):
            raise AssertionError(f"Unexpected encoded dimensions: {image.size}")
        if abs(width * wireframe["height"] - height * wireframe["width"]) > max(wireframe["width"], wireframe["height"]):
            raise AssertionError("JPEG and viewport aspect ratios differ")
        expected = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
        for index, color in enumerate(expected):
            pixel = image.getpixel((int(width * (0.25 + 0.5 * (index % 2))), int(height * (0.25 + 0.5 * (index // 2)))))
            if any(abs(actual - wanted) > 45 for actual, wanted in zip(pixel, color)):
                return None
        if expected_aspect and abs(width * expected_aspect[1] - height * expected_aspect[0]) > max(expected_aspect):
            return None
    return jpeg


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    launch = parser.add_mutually_exclusive_group(required=True)
    launch.add_argument("--editor", help="UnrealEditor (rendering enabled), not a commandlet")
    launch.add_argument("--game-executable", help="Packaged PostHogTestHost executable")
    parser.add_argument("--mock-server", required=True)
    parser.add_argument("--renderer", choices=["software", "native"], default="software")
    parser.add_argument("--manual-start", action="store_true", help="Verify no frames before an explicit Blueprint replay start")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/replay-smoke" / str(uuid.uuid4()))
    parser.add_argument("--timeout", type=float, default=180)
    return parser


def launch_command(args, system, output):
    command = [args.editor, str(PROJECT)] if args.editor else [args.game_executable]
    command += ["-game", "-PostHogReplaySmoke", "-windowed", "-ResX=640", "-ResY=480",
                "-ForceRes", "-unattended", "-nosound", "-nosplash", "-nop4", "-NoAssetRegistryCache",
                "-NoScreenMessages", "-ExecCmds=t.MaxFPS 15", f"-abslog={output / 'editor.log'}",
                "-stdout", "-FullStdOutLogOutput", *renderer_arguments(system, args.renderer)]
    if args.manual_start:
        command.append("-PostHogReplayManualStart")
    # Xvfb supplies a window/present path; lavapipe performs rendering on the CPU.
    if system == "Linux":
        command = ["xvfb-run", "-a", "-s", "-screen 0 800x600x24", *command]
    return command


def main():
    parser = argument_parser()
    args = parser.parse_args()
    system = platform.system()
    output = args.output.resolve()
    command = launch_command(args, system, output)
    if system == "Linux" and args.renderer == "software" and not (os.environ.get("VK_DRIVER_FILES") or os.environ.get("VK_ICD_FILENAMES")):
        parser.error("Select the lavapipe ICD with VK_DRIVER_FILES or VK_ICD_FILENAMES")
    output.mkdir(parents=True, exist_ok=False)
    controls = output / "controls"
    controls.mkdir()
    dumps = output / "snapshots"
    dumps.mkdir()

    class Server(MockServerTestCase):
        mock_server_path = args.mock_server
        server_environment = {"SNAPSHOT_DUMP_DIR": str(dumps)}

    Server.start_server()
    process = None
    try:
        Server.http("/__control/reset", {})
        Server.http("/__control/state", {"sessionReplayEnabled": True})
        env = {**os.environ, "POSTHOG_TEST_HOST": Server.host, "POSTHOG_TEST_STORAGE": str(output / "state"),
               "POSTHOG_TEST_CONTROL_DIRECTORY": str(controls)}
        with (output / "stdout.log").open("w") as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + args.timeout
            matching = []
            ready = None
            ready_at = None
            initial_frames = []
            resized_frames = []
            stable_count = None
            stable_since = None
            resumed_at_count = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError(f"Unreal exited before screenshots were verified: {process.returncode}")
                events = Server.http("/__captured/events")
                ready_events = [e for e in events if e["event"] == "replay_smoke_ready"]
                if ready_events:
                    if len(ready_events) != 1:
                        raise AssertionError("Expected exactly one readiness event")
                    ready = ready_events[0]
                    if ready["properties"].get("capture_mode") != "automatic-viewport":
                        raise AssertionError("Expected production automatic viewport readback")
                    if args.manual_start and not (controls / "start").exists():
                        if list(dumps.glob("*.json")):
                            raise AssertionError("Replay uploaded frames before explicit start")
                        if ready_at is None:
                            ready_at = time.monotonic()
                        if time.monotonic() - ready_at < 2:
                            time.sleep(0.2)
                            continue
                        (controls / "start").touch()
                    if args.renderer == "software":
                        check_software_adapter(ready["properties"], system)
                    matching = []
                    for path in sorted(dumps.glob("*.json")):
                        try:
                            envelopes = json.loads(path.read_text())
                        except json.JSONDecodeError:
                            continue  # The mock is still writing this batch.
                        for envelope in envelopes:
                            if envelope["distinct_id"] != ready["distinct_id"] or envelope["properties"]["$session_id"] != ready["properties"]["$session_id"]:
                                raise AssertionError("Replay does not share analytics identity/session")
                            aspect = (16, 9) if initial_frames else (ready["properties"]["viewport_width"], ready["properties"]["viewport_height"])
                            jpeg = decode_pattern(envelope, aspect)
                            if jpeg is not None:
                                matching.append((envelope, jpeg))
                    if not initial_frames and len(matching) >= 3:
                        initial_frames = matching[:3]
                        (controls / "resize").touch()
                    elif initial_frames and len(matching) >= 3:
                        resized_frames = matching[:3]
                        if resumed_at_count is None:
                            (controls / "halt").touch()
                        if resumed_at_count is not None:
                            if len(Server.http("/__captured/snapshots")) >= resumed_at_count + 2:
                                break  # Exit with recording active, rather than after an idle producer.
                        elif any(event["event"] == "replay_smoke_halted" for event in events):
                            # Previously acquired work may still deliver after a manual stop.
                            count = len(Server.http("/__captured/snapshots"))
                            if count != stable_count:
                                stable_count, stable_since = count, time.monotonic()
                            elif time.monotonic() - stable_since >= 2:
                                resumed_at_count = count
                                (controls / "resume").touch()
                time.sleep(0.2)
            else:
                raise AssertionError(f"No complete automatic capture/resize/stop evidence before timeout; see {output}")
            if len({e["uuid"] for e, _ in matching}) != len(matching):
                raise AssertionError("Duplicate snapshot UUIDs")
            for index, (_, jpeg) in enumerate(initial_frames + resized_frames):
                (output / f"frame-{index}.jpg").write_bytes(jpeg)
            (controls / "stop").touch()
            code = process.wait(timeout=30)
            if code != 0:
                raise AssertionError(f"Unreal shutdown failed: {code}")
        report = {"success": True, "launch_mode": "editor" if args.editor else "packaged",
                  "capture_mode": "automatic-viewport", "renderer": args.renderer, "platform": system, "manual_start": args.manual_start,
                  "matching_frames": len(initial_frames), "resized_frames": len(resized_frames), "stopped_stable_count": stable_count, "exit_with_recording_active": resumed_at_count is not None, "adapter": ready["properties"]["rhi_adapter"],
                  "session_id": ready["properties"]["$session_id"], "exit_code": code}
        (output / "result.json").write_text(json.dumps(report, indent=2))
        print(json.dumps(report, indent=2))
    finally:
        if process is not None and process.poll() is None:
            (controls / "stop").touch()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=15)
        (output / "wire.json").write_text(json.dumps(Server.http("/__captured/all"), indent=2))
        Server.log.seek(0)
        (output / "mock.log").write_bytes(Server.log.read())
        Server.stop_server()


if __name__ == "__main__":
    main()
