# Sea-Eagle build-system snapshots

This directory archives the Sea-Eagle build files that were reconstructed while
integrating WaveTrace.  The snapshots are kept separate from the live WaveTrace
source tree so their historical state and their optimized state can be compared
without mixing either one into the product build.

## Layout

- `sea_eagle/original`: filtered mirror of the build-system extraction in
  `buildInfo_1_restored/mirror`.
- `sea_eagle/optimized`: the same mirror with the final root-build changes
  overlaid.

The original snapshot contains 130 files.  The optimized snapshot contains 131
files because it also includes `SW/projects.se/tools/bin/gcDefineGen.py`.

## Selection rules

The snapshots contain build inputs only: CMake files, Makefiles, make fragments,
batch/PowerShell/shell scripts, and Visual Studio property/target files.  Source
files and generated artifacts such as solutions, projects, objects, libraries,
executables, PDBs, CMake caches, and build directories are not included.

The following extracted files were deliberately excluded:

- third-party build files;
- `WaveTracer/integration/CMakeLists.txt`, which is not part of the Sea-Eagle
  root-build snapshot;
- the one-click extraction/scanning script itself.

The extraction traversed the Sea-Eagle `arch` directory link from both its HW
and SW views.  Both relative paths are retained in the mirror.  Therefore the
optimized cmodel CMake file is overlaid at both paths even though they refer to
one file in a normal Sea-Eagle workspace.

## Optimized overlay

The optimized snapshot differs from the original at these paths:

- `Sea-Eagle_build.bat`
- `SW/projects.se/build-gpgpu-solution.bat`
- `SW/projects.se/driver/cuda/CMakeLists.txt`
- `HW/projects.se/arch/XAQ2/cmodel/CMakeLists.txt`
- `SW/projects.se/arch/XAQ2/cmodel/CMakeLists.txt`
- `SW/projects.se/tools/bin/gcDefineGen.py` (added)

The root four-file baseline came from
`SeaEagle_root4_plus_WaveTracer_full_replace_20260704.zip`.  The cmodel CMake
file was then updated from
`SeaEagle_cmodel_reggen_solution_target_20260707.zip`, and the incremental
generator came from `SeaEagle_gcDefineGen_incremental_20260706.zip`.

The resulting build setup provides multi-configuration Visual Studio generation
(`Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`, and `Profile`), profiler
symbols for optimized configurations, `/MP32` compilation, optional 32-way
command-line builds, flat SDK output paths, generate-only root BAT behavior, and
incremental cmodel/gcDefine generation.  `cmodel_reggen` remains an explicit
solution target and does not depend on WaveTrace's ReflectGen target.

## Validation boundary

The two trees are archival mirrors, not standalone Sea-Eagle source trees.
Their file sets, exclusions, overlay differences, configuration markers, and
absence of generated artifacts are validated in this repository.  A complete
configure/build still requires the proprietary Sea-Eagle source tree, tools,
SDKs, link targets, and the Visual Studio 2019 environment used by the root BAT.
