# Neon Runner

A playable three-lane infinite runner demonstrating the PostHog Unreal SDK. The track, robot, obstacles, coins, lighting, and HUD are built from engine primitives at runtime. There are no project content assets to download or generate.

## Build and play

Current validated target: **Unreal Engine 5.8 on macOS arm64**. Follow the repository's [native build prerequisites](../../README.md#building-the-development-checkout), including the development Rust embedding checkout.

From the repository root:

```sh
export UE_ROOT="/path/to/UE_5.8"
export POSTHOG_RS_PATH="/path/to/posthog-rs-embedding-checkout"
export PYTHON=python3 # Python 3.11+ for the native build
./scripts/build-native.sh aarch64-apple-darwin
./scripts/build-example.sh
./scripts/run-example.sh
```

In the editor, press **Play**, click the game viewport, and wait for the three-second countdown. Forward movement is automatic.

| Control | Action |
| --- | --- |
| A / Left | Switch one lane left |
| D / Right | Switch one lane right |
| Space / Up | Jump |
| S / Down | Slide |
| R / Enter / Space after game over | Restart |

Jump gold hurdles, slide under cyan beams, and dodge red walls. Every obstacle row has an open lane marked by coins. Speed increases with distance, and old track sections are retired as new ones appear.

After building, you can open `PostHogExample.uproject` directly or use the launch script. Project defaults select the engine's empty Entry map, `RunnerGameMode`, and `RunnerGameInstance`. The track and robot appear when you press **Play**, not in the editing viewport. Keep machine-specific overrides in ignored `Config/UserEngine.ini`. Restart the editor after changing project defaults or rebuilding C++; the SDK does not support Live Coding/module reloading.

## Analytics

**The game works without credentials.** To enable capture, launch the script from a terminal with `POSTHOG_TOKEN` set. `POSTHOG_HOST` optionally overrides the default ingestion host. Do not put real credentials in tracked files or command-line arguments. An SDK instance already configured through Project Settings retains that configuration; enable editor capture there when using PIE.

For a local test, start the maintained SDK mock server, then use its dummy token:

```sh
export POSTHOG_TOKEN=phc_MOCK
export POSTHOG_HOST=http://127.0.0.1:9999
./scripts/run-example.sh
```

The HUD shows whether the client is disabled, initializing, ready, or failed. Ready means initialized, not that the server has accepted every event.

| Event | When |
| --- | --- |
| `run_started` | Countdown finishes |
| `coin_collected` | A coin is collected |
| `run_ended` | Collision, or leaving a started run |

Events include `run_id`, `difficulty`, `distance_meters`, `coins`, `duration_seconds`, `speed_cm_per_second`, and the target `lane` (-1, 0, 1). `run_ended.reason` is `collision` or `abandoned`. Run IDs change on restart; the SDK's anonymous identity, session, and client survive because they belong to the game instance, not the level. Early events are buffered in memory during initialization (up to 64); a failed setup drops them. The subsystem owns initialization/shutdown, and delivery never blocks the game thread.

### Difficulty flag

Create the boolean flag **`runner-hard-mode`**. When true, the initial speed increases from 900 to 1050 cm/s; both modes cap at 1800 cm/s. The game reads the cached value once per run, records the flag exposure through the SDK, and uses normal mode if no value is available. Late flag results affect the next run rather than changing difficulty mid-run.

With the mock server:

```sh
curl -X POST http://127.0.0.1:9999/__control/state \
  -H 'Content-Type: application/json' \
  -d '{"flags":{"runner-hard-mode":true}}'
```

Restart PIE to fetch the new response, or reload flags through the SDK before the next run. Changing the mock response alone does not update the client's cached flags.

## Optional character art

The built-in articulated robot makes the example runnable without a skeletal mesh or animation pack. To use your own art, derive a Blueprint from `RunnerCharacter`, assign its inherited skeletal mesh and animation class, and select that Blueprint as the game mode's default pawn. The procedural robot hides itself when a skeletal mesh is assigned. A production character should provide its own crouch/slide animation.

## Validation

After building, run the three rule tests in Unreal's console:

```
Automation RunTests PostHogExample.Runner
```

For Play-in-Editor gameplay and event-ingestion checks, use the maintained mock server implementation:

```sh
python3 Tests/runner_integration.py \
  --ue-root "$UE_ROOT" \
  --mock-server /path/to/sdk-mock-server/server.py
```

The driver launches the same public run script, enables Python only for the test process, and checks the unconfigured game, normal/hard flags, a late flag response, a flag-request timeout, real obstacle traversal, track cleanup, game over, and restart. The late-response scenario shortens the first countdown to ensure the run starts before the response arrives. It inspects ingested events, including run correlation and identity/session continuity. Logs, results, and captured mock events are written to ignored `artifacts/runner/`.
