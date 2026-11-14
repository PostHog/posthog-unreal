"""Literal include boundaries for the thin adapter; UHT/compiler checks resolve engine/generated headers."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1] / "Source"
OWNERS = {
    "Private/PostHogBlueprintLibrary.cpp": "Blueprint value conversion",
    "Private/PostHogClient.cpp": "SDK delegation and native lifetime",
    "Private/PostHogJson.h": "JSON codec",
    "Private/PostHogModule.cpp": "module composition/teardown",
    "Private/PostHogNative.cpp": "ABI value conversion",
    "Private/PostHogNative.h": "ABI value conversion",
    "Private/PostHogProperties.cpp": "owned JSON values",
    "Private/PostHogReplay.cpp": "engine pixel preparation",
    "Private/PostHogReplay.h": "engine pixel preparation",
    "Private/PostHogReplayRecorder.cpp": "owning viewport/GPU/worker lifetime",
    "Private/PostHogReplayRecorder.h": "engine capture composition surface",
    "Private/PostHogReadbackSlot.h": "retired readback allocation compatibility",
    "Private/PostHogReplayWork.cpp": "bounded host lease staging",
    "Private/PostHogReplayWork.h": "bounded host lease staging",
    "Private/PostHogReplayFrame.cpp": "Blueprint lease lifetime",
    "Private/PostHogSettings.cpp": "engine configuration",
    "Private/PostHogSubsystem.cpp": "game-instance/lifecycle orchestration",
    "Private/PostHogTasks.cpp": "tracked host work",
    "Private/PostHogTasks.h": "tracked host work",
    "Public/PostHogBlueprintLibrary.h": "Blueprint value surface",
    "Public/PostHogClient.h": "native adapter surface",
    "Public/PostHogFeatureFlags.h": "SDK snapshot presentation",
    "Public/PostHogProperties.h": "owned JSON value surface",
    "Public/PostHogReplayFrame.h": "Blueprint lease surface",
    "Public/PostHogSettings.h": "engine configuration surface",
    "Public/PostHogSubsystem.h": "game-instance surface",
    "Public/PostHogTypes.h": "SDK/adapter outcome presentation",
}
OWNERS.update({
    "Private/PostHogAnalyticsProvider.cpp": "Unreal Analytics event/session annotations",
    "Public/PostHogAnalyticsProvider.h": "Unreal Analytics adapter surface",
})
OWNERS = {"PostHog/" + path: responsibility for path, responsibility in OWNERS.items()}
OWNERS["PostHogAnalytics/Private/PostHogAnalyticsModule.cpp"] = "bound Analytics factory"
ABI_INCLUDES = {"posthog_unreal.h", "PostHogNative.h"}
ABI_OWNERS = {"Private/PostHogClient.cpp", "Private/PostHogNative.h", "Private/PostHogNative.cpp", "Private/PostHogModule.cpp"}
STORAGE_INCLUDES = {"Misc/FileHelper.h", "HAL/FileManager.h", "HAL/PlatformFileManager.h", "fstream", "filesystem"}


def validate(sources):
    names = {Path(path).name: path for path in sources}
    if len(names) != len(sources):
        raise ValueError("ambiguous production include names")
    graph = {path: set() for path in sources}
    for path, text in sources.items():
        unit = path.removeprefix("PostHog/")
        for include in re.findall(r'^\s*#include\s*["<]([^">]+)[">]', text, re.MULTILINE):
            if include in ABI_INCLUDES and unit not in ABI_OWNERS:
                raise ValueError(f"direct ABI dependency outside native boundary: {path}")
            if unit in {"Private/PostHogClient.cpp", "Private/PostHogAnalyticsProvider.cpp"} and include in STORAGE_INCLUDES:
                raise ValueError("SDK persistence belongs to Rust, not the client adapter")
            if unit == "Private/PostHogSubsystem.cpp" and include in {"RHIGPUReadback.h", "RHICommandList.h", "PostHogReplayWork.h"}:
                raise ValueError("GPU staging belongs to the recorder, not UObject orchestration")
            target = names.get(Path(include).name)
            if target:
                graph[path].add(target)
    visiting, done = set(), set()

    def visit(path):
        if path in visiting:
            raise ValueError(f"production include cycle at {path}")
        if path in done:
            return
        visiting.add(path)
        for dependency in graph[path]:
            visit(dependency)
        visiting.remove(path)
        done.add(path)

    for path in graph:
        visit(path)


class ArchitectureTests(unittest.TestCase):
    def test_production_graph_and_classification(self):
        sources = {str(path.relative_to(ROOT)): path.read_text() for path in ROOT.rglob("*")
                   if path.suffix in (".h", ".cpp") and "Tests" not in path.relative_to(ROOT).parts
                   and path.relative_to(ROOT).parts[0] in {"PostHog", "PostHogAnalytics"}}
        self.assertEqual(set(sources), set(OWNERS), "Classify each new production unit by responsibility")
        validate(sources)

    def test_direct_abi_and_persistence_violations_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "direct ABI dependency"):
            validate({"Private/PostHogSubsystem.cpp": '#include "posthog_unreal.h"'})
        with self.assertRaisesRegex(ValueError, "persistence belongs to Rust"):
            validate({"Private/PostHogClient.cpp": '#include "Misc/FileHelper.h"'})
        validate({"Private/PostHogClient.cpp": '#include "PostHogNative.h"'})
        with self.assertRaisesRegex(ValueError, "direct ABI dependency"):
            validate({"PostHogAnalytics/Private/PostHogAnalyticsModule.cpp": '#include "posthog_unreal.h"'})
        with self.assertRaisesRegex(ValueError, "persistence belongs to Rust"):
            validate({"PostHog/Private/PostHogAnalyticsProvider.cpp": '#include "Misc/FileHelper.h"'})
        with self.assertRaisesRegex(ValueError, "GPU staging belongs to the recorder"):
            validate({"Private/PostHogSubsystem.cpp": '#include "RHIGPUReadback.h"'})
        validate({"Private/PostHogSubsystem.cpp": '#include "PostHogReplayRecorder.h"'})

    def test_cycles_are_rejected_and_public_entry_dependencies_pass(self):
        with self.assertRaisesRegex(ValueError, "include cycle"):
            validate({"Public/PostHogClient.h": '#include "PostHogSubsystem.h"',
                      "Public/PostHogSubsystem.h": '#include "PostHogClient.h"'})
        validate({"Public/PostHogClient.h": "", "Public/PostHogSubsystem.h": '#include "PostHogClient.h"'})
