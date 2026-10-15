import unreal
import json
import time
import traceback
import os
from pathlib import Path

unreal.EditorPythonScripting.set_keep_python_script_alive(True)
out = Path(os.environ["RUNNER_TEST_OUTPUT"])
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
results = []
state = {"phase": "start", "at": time.monotonic(), "started": time.monotonic()}
handle = None


def check(name, condition, detail=None):
    results.append({"check": name, "passed": bool(condition), "detail": detail})
    assert condition, name + ": " + str(detail)


def phase(name):
    state.update(phase=name, at=time.monotonic())
    out.joinpath("smoke-progress.json").write_text(json.dumps(state))


def save(success, error=None):
    out.joinpath("smoke-result.json").write_text(
        json.dumps({"success": success, "checks": results, "error": error}, indent=2)
    )


def track_segments(world):
    cls = unreal.load_class(None, "/Script/PostHogExample.RunnerTrackSegment")
    return unreal.GameplayStatics.get_all_actors_of_class(world, cls)


def hazards_in(run, world, pawn, kind):
    ahead = run.track_distance_at(pawn.get_actor_location()) + 1000
    matches = []
    for segment in track_segments(world):
        for c in segment.get_components_by_class(unreal.StaticMeshComponent):
            if (
                not c.component_has_tag("RunnerHazard")
                or run.track_distance_at(c.get_world_location()) < ahead
            ):
                continue
            height = c.get_editor_property("relative_scale3d").z
            if (
                (kind == "hurdle" and height < 1)
                or (kind == "beam" and 1 < height < 2)
                or (kind == "wall" and height >= 2.3)
            ):
                matches.append(c)
    check(kind + " obstacles are generated", bool(matches))
    return min(matches, key=lambda c: run.track_distance_at(c.get_world_location()))


def place_before(run, pawn, hazard):
    pos = hazard.get_world_location()
    lane = run.track_lane_at(pos)
    while pawn.target_lane > lane:
        pawn.move_left()
    while pawn.target_lane < lane:
        pawn.move_right()
    state["hazard_d"] = run.track_distance_at(pos)
    pawn.set_actor_location(
        run.track_location(state["hazard_d"] - 400, lane, 98), False, True
    )


def travelled(run, pawn):
    return run.track_distance_at(pawn.get_actor_location())


def tick(delta):
    try:
        now = time.monotonic()
        if now - state["started"] > 90:
            raise RuntimeError("Timed out in " + state["phase"])
        p = state["phase"]
        age = now - state["at"]
        if p == "start":
            check(
                "Default editor map is Entry",
                editor.get_editor_world().get_path_name() == "/Engine/Maps/Entry.Entry",
            )
            levels.editor_request_begin_play()
            phase("countdown")
            return
        world = editor.get_game_world()
        if p == "done":
            if not levels.is_in_play_in_editor():
                save(True)
                unreal.unregister_slate_post_tick_callback(handle)
                unreal.EditorPythonScripting.set_keep_python_script_alive(False)
            return
        if not world:
            return
        run = unreal.GameplayStatics.get_game_mode(world)
        pawn = unreal.GameplayStatics.get_player_character(world, 0)
        if not run or not pawn:
            if age > 3:
                raise RuntimeError(
                    "Missing gameplay actors: "
                    + str(
                        {
                            "world": str(world),
                            "run": str(run),
                            "pawn": str(pawn),
                            "actors": [
                                a.get_class().get_name()
                                for a in unreal.GameplayStatics.get_all_actors_of_class(
                                    world, unreal.Actor
                                )
                            ],
                        }
                    )
                )
            return
        if (
            p == "forward"
            and run.is_running()
            and os.environ.get("RUNNER_SHORT_COUNTDOWN") == "1"
        ):
            run.set_editor_property("custom_time_dilation", 1.0)
        if p == "countdown" and age > 0.5:
            check(
                "Runner mode and pawn are active",
                type(run).__name__ == "RunnerGameMode"
                and type(pawn).__name__ == "RunnerCharacter",
            )
            check(
                "Countdown holds player at start",
                abs(pawn.get_actor_location().x - 600) < 10,
                str(pawn.get_actor_location()),
            )
            check(
                "Title menu holds the countdown",
                run.countdown >= 3 and not run.is_running(),
                run.countdown,
            )
            if os.environ.get("RUNNER_SHORT_COUNTDOWN") == "1":
                run.set_editor_property("custom_time_dilation", 4.0)
            run.start_run()
            phase("forward")
        elif p == "forward" and run.is_running() and run.distance_meters > 5:
            check(
                "Automatically runs forward",
                run.distance_meters > 5,
                run.distance_meters,
            )
            check(
                "Difficulty matches the cached flag",
                run.hard_mode == (os.environ.get("RUNNER_EXPECT_HARD") == "1"),
                run.hard_mode,
            )
            check("Coins collected during movement", run.coins > 0, run.coins)
            state["x"] = pawn.get_actor_location().x
            pawn.move_left()
            pawn.move_left()
            check(
                "Left input clamps at outer lane",
                pawn.target_lane == -1,
                pawn.target_lane,
            )
            phase("left")
        elif p == "left" and age > 0.3:
            check(
                "Smoothly reaches left lane",
                abs(pawn.get_actor_location().y + 300) < 2,
                str(pawn.get_actor_location()),
            )
            check(
                "Lane switch preserves forward movement",
                pawn.get_actor_location().x > state["x"],
            )
            pawn.move_right()
            pawn.move_right()
            pawn.move_right()
            check(
                "Right input clamps at outer lane",
                pawn.target_lane == 1,
                pawn.target_lane,
            )
            phase("right")
        elif p == "right" and age > 0.5:
            check(
                "Smoothly reaches right lane",
                abs(pawn.get_actor_location().y - 300) < 2,
                str(pawn.get_actor_location()),
            )
            pawn.set_actor_location(unreal.Vector(600, 300, 98), False, True)
            phase("settle")
        elif p == "settle" and age > 0.15:
            pawn.slide()
            phase("slide")
        elif p == "slide" and age > 0.15:
            check(
                "Slide shrinks collision capsule",
                pawn.capsule_component.get_scaled_capsule_half_height() < 60,
                pawn.capsule_component.get_scaled_capsule_half_height(),
            )
            check("Slide silhouette is active", pawn.sliding)
            phase("stand")
        elif p == "stand" and age > 0.9:
            check(
                "Slide restores standing capsule",
                pawn.capsule_component.get_scaled_capsule_half_height() > 90,
                pawn.capsule_component.get_scaled_capsule_half_height(),
            )
            check("Slide state resets", not pawn.sliding)
            state["ground_z"] = pawn.get_actor_location().z
            pawn.jump_or_restart()
            phase("jump")
        elif p == "jump" and age > 0.2:
            check(
                "Jump leaves ground",
                pawn.get_actor_location().z > state["ground_z"] + 40,
                str(pawn.get_actor_location()),
            )
            phase("land")
        elif p == "land" and age > 0.7:
            check(
                "Jump lands back on track",
                pawn.character_movement.is_moving_on_ground(),
            )
            corners = [s for s in track_segments(world) if s.turn != 0]
            check(
                "A corner is generated within the first 80000 cm",
                corners and min(s.start_distance for s in corners) < 80000,
                [(s.start_distance, s.turn) for s in corners],
            )
            corner = min(corners, key=lambda s: s.start_distance)
            state["corner_end"] = corner.start_distance + corner.track_length
            state["corner_yaw"] = corner.get_actor_rotation().yaw + 90 * corner.turn
            pawn.set_actor_location(
                run.track_location(corner.start_distance - 400, pawn.target_lane, 98),
                False,
                True,
            )
            phase("corner")
        elif p == "corner" and (
            run.game_over or travelled(run, pawn) > state["corner_end"] + 200 or age > 4
        ):
            check(
                "Runner follows the track through a corner",
                not run.game_over and age < 4,
                str(pawn.get_actor_location()),
            )
            check(
                "Runner keeps its lane through a corner",
                run.track_lane_at(pawn.get_actor_location()) == pawn.target_lane,
                str(pawn.get_actor_location()),
            )
            check(
                "Runner turns to face the new direction",
                abs(
                    (pawn.get_actor_rotation().yaw - state["corner_yaw"] + 180) % 360
                    - 180
                )
                < 5,
                pawn.get_actor_rotation().yaw,
            )
            pawn.set_actor_location(run.track_location(80000, 1, 98), False, True)
            phase("stream")
        elif p == "stream" and age > 0.12:
            segments = track_segments(world)
            check(
                "Track generation stays bounded after long travel",
                7 <= len(segments) <= 16,
                len(segments),
            )
            check(
                "Old track is retired",
                all(s.start_distance > 70000 for s in segments),
            )
            check(
                "Speed increases with distance",
                run.get_run_speed() > 1000,
                run.get_run_speed(),
            )
            place_before(run, pawn, hazards_in(run, world, pawn, "hurdle"))
            pawn.jump_or_restart()
            phase("clear_hurdle")
        elif p == "clear_hurdle" and age > 0.85:
            check(
                "Jump clears an actual low obstacle",
                not run.game_over and travelled(run, pawn) > state["hazard_d"] + 100,
                str(pawn.get_actor_location()),
            )
            place_before(run, pawn, hazards_in(run, world, pawn, "beam"))
            pawn.slide()
            phase("clear_beam")
        elif p == "clear_beam" and age > 1.0:
            check(
                "Slide clears an actual overhead beam",
                not run.game_over and travelled(run, pawn) > state["hazard_d"] + 100,
                str(pawn.get_actor_location()),
            )
            check(
                "Player stands after passing beam",
                not pawn.sliding
                and pawn.capsule_component.get_scaled_capsule_half_height() > 90,
            )
            place_before(run, pawn, hazards_in(run, world, pawn, "wall"))
            phase("collision")
        elif p == "collision" and run.game_over:
            check("Hazard collision ends run", run.game_over)
            stopped = pawn.get_actor_location()
            state["stopped"] = [stopped.x, stopped.y, stopped.z]
            state["distance"] = run.distance_meters
            state["coins"] = run.coins
            phase("stopped")
        elif p == "stopped" and age > 0.3:
            check(
                "Game over stops movement",
                pawn.get_actor_location().distance(unreal.Vector(*state["stopped"]))
                < 1,
            )
            pawn.move_left()
            pawn.slide()
            run.collect_coin()
            check("Game over freezes score", run.coins == state["coins"])
            pawn.retry_run()
            phase("restart")
        elif p == "restart" and age > 0.6 and not run.game_over:
            check("Restart creates a fresh countdown", run.countdown > 0, run.countdown)
            check(
                "Restart resets distance and coins",
                run.distance_meters < 1 and run.coins == 0,
            )
            check("Restart resets lane", pawn.target_lane == 0)
            phase("second_run")
        elif p == "second_run" and run.is_running() and run.distance_meters > 3:
            check("Restart begins another run", not run.game_over)
            check(
                "Restart uses current cached difficulty",
                run.hard_mode == (os.environ.get("RUNNER_EXPECT_SECOND_HARD") == "1"),
                run.hard_mode,
            )
            run.end_run()
            phase("finish")
        elif p == "finish" and age > 0.3:
            levels.editor_request_end_play()
            phase("done")
    except Exception:
        save(False, traceback.format_exc())
        unreal.log_error(traceback.format_exc())
        unreal.unregister_slate_post_tick_callback(handle)
        levels.editor_request_end_play()
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)


handle = unreal.register_slate_post_tick_callback(tick)
