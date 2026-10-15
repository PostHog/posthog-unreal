import unreal
import json
import time
import traceback
import os
from pathlib import Path

unreal.EditorPythonScripting.set_keep_python_script_alive(True)
out = Path(os.environ['RUNNER_TEST_OUTPUT'])
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
results = []
state = {'phase': 'start', 'at': time.monotonic(), 'started': time.monotonic()}
handle = None

def check(name, condition, detail=None):
    results.append({'check': name, 'passed': bool(condition), 'detail': detail})
    assert condition, name + ': ' + str(detail)

def phase(name):
    state.update(phase=name, at=time.monotonic())
    out.joinpath('smoke-progress.json').write_text(json.dumps(state))

def save(success, error=None):
    out.joinpath('smoke-result.json').write_text(json.dumps({'success': success, 'checks': results, 'error': error}, indent=2))

def hazards_in(world, pawn, kind):
    cls = unreal.load_class(None, '/Script/PostHogExample.RunnerTrackSegment')
    segments = unreal.GameplayStatics.get_all_actors_of_class(world, cls)
    matches = []
    for segment in segments:
        for c in segment.get_components_by_class(unreal.StaticMeshComponent):
            if not c.component_has_tag('RunnerHazard') or c.get_world_location().x < pawn.get_actor_location().x + 1000:
                continue
            height = c.get_editor_property('relative_scale3d').z
            if (kind == 'hurdle' and height < 1) or (kind == 'beam' and 1 < height < 2) or (kind == 'wall' and height >= 2.3):
                matches.append(c)
    check(kind + ' obstacles are generated', bool(matches))
    return min(matches, key=lambda c: c.get_world_location().x)

def place_before(pawn, hazard):
    pos = hazard.get_world_location()
    lane = round(pos.y / 300)
    while pawn.target_lane > lane: pawn.move_left()
    while pawn.target_lane < lane: pawn.move_right()
    pawn.set_actor_location(unreal.Vector(pos.x - 400, pos.y, 98), False, True)
    state['hazard_x'] = pos.x

def tick(delta):
    try:
        now = time.monotonic()
        if now - state['started'] > 90:
            raise RuntimeError('Timed out in ' + state['phase'])
        p = state['phase']
        age = now - state['at']
        if p == 'start':
            check('Default editor map is Entry', editor.get_editor_world().get_path_name() == '/Engine/Maps/Entry.Entry')
            levels.editor_request_begin_play()
            phase('countdown')
            return
        world = editor.get_game_world()
        if p == 'done':
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
                raise RuntimeError('Missing gameplay actors: ' + str({'world': str(world), 'run': str(run), 'pawn': str(pawn), 'actors': [a.get_class().get_name() for a in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor)]}))
            return
        if p == 'forward' and run.is_running() and os.environ.get('RUNNER_SHORT_COUNTDOWN') == '1':
            run.set_editor_property('custom_time_dilation', 1.0)
        if p == 'countdown' and age > 0.5:
            check('Runner mode and pawn are active', type(run).__name__ == 'RunnerGameMode' and type(pawn).__name__ == 'RunnerCharacter')
            check('Countdown holds player at start', abs(pawn.get_actor_location().x - 600) < 10, str(pawn.get_actor_location()))
            if os.environ.get('RUNNER_SHORT_COUNTDOWN') == '1':
                run.set_editor_property('custom_time_dilation', 4.0)
            phase('forward')
        elif p == 'forward' and run.is_running() and run.distance_meters > 5:
            check('Automatically runs forward', run.distance_meters > 5, run.distance_meters)
            check('Difficulty matches the cached flag', run.hard_mode == (os.environ.get('RUNNER_EXPECT_HARD') == '1'), run.hard_mode)
            check('Coins collected during movement', run.coins > 0, run.coins)
            state['x'] = pawn.get_actor_location().x
            pawn.move_left()
            pawn.move_left()
            check('Left input clamps at outer lane', pawn.target_lane == -1, pawn.target_lane)
            phase('left')
        elif p == 'left' and age > 0.3:
            check('Smoothly reaches left lane', abs(pawn.get_actor_location().y + 300) < 2, str(pawn.get_actor_location()))
            check('Lane switch preserves forward movement', pawn.get_actor_location().x > state['x'])
            pawn.move_right()
            pawn.move_right()
            pawn.move_right()
            check('Right input clamps at outer lane', pawn.target_lane == 1, pawn.target_lane)
            phase('right')
        elif p == 'right' and age > 0.5:
            check('Smoothly reaches right lane', abs(pawn.get_actor_location().y - 300) < 2, str(pawn.get_actor_location()))
            pawn.set_actor_location(unreal.Vector(600, 300, 98), False, True)
            phase('settle')
        elif p == 'settle' and age > 0.15:
            pawn.slide()
            phase('slide')
        elif p == 'slide' and age > 0.15:
            check('Slide shrinks collision capsule', pawn.capsule_component.get_scaled_capsule_half_height() < 60,
                  pawn.capsule_component.get_scaled_capsule_half_height())
            check('Slide silhouette is active', pawn.sliding)
            phase('stand')
        elif p == 'stand' and age > 0.9:
            check('Slide restores standing capsule', pawn.capsule_component.get_scaled_capsule_half_height() > 90,
                  pawn.capsule_component.get_scaled_capsule_half_height())
            check('Slide state resets', not pawn.sliding)
            state['ground_z'] = pawn.get_actor_location().z
            pawn.jump_or_restart()
            phase('jump')
        elif p == 'jump' and age > 0.2:
            check('Jump leaves ground', pawn.get_actor_location().z > state['ground_z'] + 40, str(pawn.get_actor_location()))
            phase('land')
        elif p == 'land' and age > 0.7:
            check('Jump lands back on track', pawn.character_movement.is_moving_on_ground())
            pawn.set_actor_location(unreal.Vector(80000, 300, 98), False, True)
            phase('stream')
        elif p == 'stream' and age > 0.12:
            cls = unreal.load_class(None, '/Script/PostHogExample.RunnerTrackSegment')
            segments = unreal.GameplayStatics.get_all_actors_of_class(world, cls)
            check('Track generation stays bounded after long travel', 7 <= len(segments) <= 12, len(segments))
            check('Old track is retired', all(s.get_actor_location().x > 70000 for s in segments))
            check('Speed increases with distance', run.get_run_speed() > 1000, run.get_run_speed())
            place_before(pawn, hazards_in(world, pawn, 'hurdle'))
            pawn.jump_or_restart()
            phase('clear_hurdle')
        elif p == 'clear_hurdle' and age > 0.85:
            check('Jump clears an actual low obstacle', not run.game_over and pawn.get_actor_location().x > state['hazard_x'] + 100,
                  str(pawn.get_actor_location()))
            place_before(pawn, hazards_in(world, pawn, 'beam'))
            pawn.slide()
            phase('clear_beam')
        elif p == 'clear_beam' and age > 1.0:
            check('Slide clears an actual overhead beam', not run.game_over and pawn.get_actor_location().x > state['hazard_x'] + 100,
                  str(pawn.get_actor_location()))
            check('Player stands after passing beam', not pawn.sliding and pawn.capsule_component.get_scaled_capsule_half_height() > 90)
            place_before(pawn, hazards_in(world, pawn, 'wall'))
            phase('collision')
        elif p == 'collision' and run.game_over:
            check('Hazard collision ends run', run.game_over)
            state['stopped_x'] = pawn.get_actor_location().x
            state['distance'] = run.distance_meters
            state['coins'] = run.coins
            phase('stopped')
        elif p == 'stopped' and age > 0.3:
            check('Game over stops movement', abs(pawn.get_actor_location().x - state['stopped_x']) < 1)
            pawn.move_left()
            pawn.slide()
            run.collect_coin()
            check('Game over freezes score', run.coins == state['coins'])
            pawn.retry_run()
            phase('restart')
        elif p == 'restart' and age > 0.6 and not run.game_over:
            check('Restart creates a fresh countdown', run.countdown > 0, run.countdown)
            check('Restart resets distance and coins', run.distance_meters < 1 and run.coins == 0)
            check('Restart resets lane', pawn.target_lane == 0)
            phase('second_run')
        elif p == 'second_run' and run.is_running() and run.distance_meters > 3:
            check('Restart begins another run', not run.game_over)
            check('Restart uses current cached difficulty', run.hard_mode == (os.environ.get('RUNNER_EXPECT_SECOND_HARD') == '1'), run.hard_mode)
            run.end_run()
            phase('finish')
        elif p == 'finish' and age > 0.3:
            levels.editor_request_end_play()
            phase('done')
    except Exception:
        save(False, traceback.format_exc())
        unreal.log_error(traceback.format_exc())
        unreal.unregister_slate_post_tick_callback(handle)
        levels.editor_request_end_play()
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)

handle = unreal.register_slate_post_tick_callback(tick)
