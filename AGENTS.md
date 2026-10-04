# AGENTS.md

General architecture, build, test and run guidance lives in `CLAUDE.md` and `README.md`.
Read those first. This file only adds Cursor Cloud specific, non-obvious notes.

## Cursor Cloud specific instructions

### Build natively, not in Docker
`README.md` / `CLAUDE.md` describe a container-first workflow (`./bin/container-validate.sh`)
because the usual dev machine is macOS. The Cloud VM is already Linux, so build/test/run
happen **directly on the VM** with the same dependencies the dev image installs. Do not try
to run Docker-in-Docker here.

The update script only refreshes git submodules (`git submodule update --init --recursive`).
System dependencies (build toolchain, protobuf, yaml-cpp, sqlite/hiredis, OpenSSL,
`mosquitto-clients`) and the from-source `llhttp v6.0.7` install into `/usr/local` are already
baked into the VM snapshot; they do not need reinstalling each session.

### Toolchain gotcha: cc/c++ point to gcc
The base image defaults `cc`/`c++` to clang, which cannot link `libstdc++` in this VM
(`cannot find -lstdc++`). The alternatives are set to gcc/g++ so CMake auto-selects gcc.
If you ever hit a `-lstdc++` link error, verify `c++ --version` reports g++.

### Build / test / run (native)
```bash
cmake -S . -B build-container -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-container -- -j"$(nproc)"
ctest --test-dir build-container --output-on-failure --timeout 120
./build-container/mqtts -c mqtts.yaml        # broker on 0.0.0.0:1883
```
- CMake `configure` re-applies `3rd/libco.patch` to the libco submodule each run (adds the
  missing `co_comm.cpp` to the static lib). This leaves the `3rd/libco` submodule with a
  modified working tree — that is expected; do not commit the submodule change.
- The linker prints harmless `coctx_swap.S.o: missing .note.GNU-stack section` warnings.

### MQTT client gotcha
The broker rejects connections with an empty/auto-generated Client Identifier
(`Client Identifier not valid`). Always pass an explicit client id, e.g.
`mosquitto_sub -i sub_x ...` / `mosquitto_pub -i pub_x ...`.

Quick end-to-end check (broker must be running):
```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -V mqttv5 -i sub_hello -t hello/world -C 1 -v &
mosquitto_pub -h 127.0.0.1 -p 1883 -V mqttv5 -i pub_hello -t hello/world -m hi -q 1
```

### Lint
There is no enforced lint/CI target. `clang-format` (config in `.clang-format`) and
`clang-tidy` are available for manual use; existing sources have pre-existing formatting
deviations, so a repo-wide `clang-format --dry-run` will report violations.
