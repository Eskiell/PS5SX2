#!/usr/bin/env bash
# PCSX2 PS5 port -- link and sign the Vulkan build (Makefile.vk's objects).
#
# This is conv.sh, build 285's link, with the GL runtime (libPS5OpenGLCore33 and
# its AGC gate) replaced by the Vulkan driver, linked the way mihawk-99's vkQuake
# port links it (PS5_vkQuake tools/build-title.sh):
#   - the four driver archives under --whole-archive,
#   - the three Mesa utility objects they need at an executable link
#     (tools/build-mesa-util.sh),
#   - undefined weak symbols resolved to 0 at link time, not imported: the driver's
#     entry-point tables name every Vulkan command weakly and the native link
#     accepts only imports a stub module exports. vkQuake passes
#     -z nodynamic-undefined-weak for this; LLD 18 does not know that option, and
#     --no-dynamic-linker gives the same result there (no PT_INTERP either way:
#     ps5-pie.ld declares the program headers).
# One change to the driver archive: its ps5vk_instance.o defines a function named
# vkGetInstanceProcAddr, and PCSX2 has a global function pointer of that name
# (VKLoader.cpp). The copy linked here renames the driver's; PCSX2's static loader
# (ORBIS_VULKAN) calls vk_icdGetInstanceProcAddr instead.
# Plus glslang for the shaderc shim (orbis-shims/ps5_shaderc.cpp).
#
#   make -f Makefile.vk -j2 && bash link-vk.sh
#
# Output: $VK_OUT/app/eboot.bin (signed), $VK_OUT/build/{llvm-pie.elf,eboot.elf,
# eboot-console-ready.elf}. PCSX2 is this repository's root; the other dependencies
# come from PS5SX2_DEPS and PS5_VULKAN_DIR (ps5/README.md), or one variable each.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
port=$(cd -- "$here/.." && pwd)
PCSX2=${PCSX2:-$(cd -- "$port/.." && pwd)}
if [[ -z ${RYML:-} || -z ${PS5_PAYLOAD_SDK:-} || -z ${GLSLANG_BUILD:-} ]]; then
  : "${PS5SX2_DEPS:?set PS5SX2_DEPS (see ps5/README.md), or RYML, PS5_PAYLOAD_SDK and GLSLANG_BUILD}"
fi
RYML=${RYML:-$PS5SX2_DEPS/ryml}
SDK=${PS5_PAYLOAD_SDK:-$PS5SX2_DEPS/sdk285}
NATIVE=${PS5_NATIVE:-$port/proto/native}
PREPARE=${PS5_PREPARE_ELF:-$port/tools/prepare_console_elf.py}
TOOL=${PS5_NATIVE_TOOL:-$NATIVE/build/host/ps5-native-tool}
VK=${PS5_VULKAN_DIR:?set PS5_VULKAN_DIR to a built Swordpdf/PS5HB_Vulkan checkout (see ps5/README.md)}
GLSLANG=${GLSLANG_BUILD:-$PS5SX2_DEPS/glslang/build-ps5}
OUT=${VK_OUT:-$port/build-vk}
TAG=${ORBIS_BUILD_TAG:-vk-285-1}
LLD=${LLD:-ld.lld-18}
OBJCOPY=${OBJCOPY:-llvm-objcopy-18}
NM=${NM:-llvm-nm-18}
export PS5_PAYLOAD_SDK=$SDK
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}

# vk-285-35: the driver and PCSX2 commits this eboot is linked from, printed in
# boot.log so a build names its exact sources (a trailing + marks uncommitted
# changes; the driver's local test-setup edit is not one of them).
src_rev() {
  local rev
  rev=$(git -C "$1" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
  if [[ -n $(git -C "$1" status --porcelain --untracked-files=no -- . \
      ':!tools/setup-test-dependencies.sh' 2>/dev/null) ]]; then
    rev="$rev+"
  fi
  echo "$rev"
}
DRIVER_REV=$(src_rev "$VK")
PCSX2_REV=$(src_rev "$PCSX2")
echo "[link-vk] sources: driver $DRIVER_REV, pcsx2 $PCSX2_REV"

mkdir -p "$OUT/build/obj" "$OUT/app/sce_sys" "$OUT/app/sce_module" "$OUT/app/assets" "$OUT/driver"

# 1. main-boot.cpp and the CRT, compiled as conv.sh compiles them (+ ORBIS_VULKAN).
sh "$NATIVE/tooling/prospero-clang18" \
  -std=c++20 -O2 -march=znver2 -msse4.1 -mavx2 -D_M_X86=1 -D__POSIX__=1 \
  -DOVERRIDE_HOST_PAGE_SIZE=0x4000 -DOVERRIDE_HOST_CACHE_LINE_SIZE=64 \
  -DORBIS_VULKAN=1 -DORBIS_BUILD_TAG="\"$TAG\"" \
  -DORBIS_DRIVER_REV="\"$DRIVER_REV\"" -DORBIS_PCSX2_REV="\"$PCSX2_REV\"" \
  -Wno-macro-redefined -Wno-unused-function -include common/Threading.h \
  -I"$here/include-orbis" -I"$PCSX2" -I"$PCSX2/pcsx2" \
  -I"$PCSX2/3rdparty/include" -I"$PCSX2/3rdparty/fmt/include" \
  -I"$PCSX2/3rdparty/fast_float/include" -I"$PCSX2/3rdparty/rapidjson/include" \
  -I"$PCSX2/3rdparty/simpleini/include" -I"$PCSX2/3rdparty/ccc/src" \
  -I"$PCSX2/3rdparty/xbyak" -I"$PCSX2/3rdparty/zydis/include" \
  -I"$PCSX2/3rdparty/zydis/dependencies/zycore/include" \
  -I"$PCSX2/3rdparty/discord-rpc/include" -I"$PCSX2/3rdparty/demangler/include" \
  -I"$PCSX2/3rdparty/rcheevos/include" -I"$PCSX2/3rdparty/imgui/include" \
  -I"$PCSX2/3rdparty/soundtouch/soundtouch" -I"$PCSX2/3rdparty/freesurround/include" \
  -I"$RYML/src" -I"$RYML/ext/c4core.src" \
  -ffunction-sections -fdata-sections \
  -c "$here/main-boot.cpp" -o "$OUT/build/obj/main-boot.o"
for item in app_crt app_cpp_runtime; do
  sh "$NATIVE/tooling/prospero-clang18" \
    -std=c++20 -O2 -Wall -Wextra \
    -ffunction-sections -fdata-sections \
    -c "$NATIVE/tooling/native/$item.cpp" -o "$OUT/build/obj/$item.o"
done
echo "[link-vk] main-boot ($TAG) and crt objects OK"

# 2. The port's objects: exactly Makefile.vk's list, in its order.
make -C "$here" -f Makefile.vk -s objects.txt PCSX2="$PCSX2" RYML="$RYML" PS5_PAYLOAD_SDK="$SDK"
OBJS="$OUT/build/obj/app_crt.o $OUT/build/obj/app_cpp_runtime.o $OUT/build/obj/main-boot.o"
while IFS= read -r o; do
  OBJS="$OBJS $here/$o"
done < "$here/objects.txt"
echo "[link-vk] $(wc -l < "$here/objects.txt") port objects"

# 3. The driver: the archive copy with the renamed entry point, the other three as built.
cp "$VK/build/driver/ps5/libps5vk.ps5.a" "$OUT/driver/libps5vk.ps5.a"
"$OBJCOPY" --redefine-sym vkGetInstanceProcAddr=ps5vk_driver_vkGetInstanceProcAddr "$OUT/driver/libps5vk.ps5.a"
if "$NM" "$OUT/driver/libps5vk.ps5.a" 2>/dev/null | grep -qE " [A-Za-z] vkGetInstanceProcAddr$"; then
  echo "[link-vk] error: the driver archive still names vkGetInstanceProcAddr" >&2; exit 1
fi
VK_ARCHIVES=(
  "$OUT/driver/libps5vk.ps5.a"
  "$VK/.deps/native/vulkan-runtime/lib/libvk_runtime.ps5.a"
  "$VK/build/driver/ps5/libpsbc_driver.ps5.a"
  "$VK/.deps/native/psbc/lib/libpsbc_support.ps5.a"
)
mapfile -t MESA_UTIL < <(PS5_VULKAN_DIR="$VK" bash "$port/tools/build-mesa-util.sh" "$OUT/mesa-util" "$SDK")
GLSLANG_LIBS=(
  "$GLSLANG/glslang/libglslang.a"
  "$GLSLANG/glslang/libglslang-default-resource-limits.a"
  "$GLSLANG/glslang/OSDependent/Unix/libOSDependent.a"
)
sha256sum "${VK_ARCHIVES[@]}" "${MESA_UTIL[@]}" "${GLSLANG_LIBS[@]}" > "$OUT/build/vk-inputs.sha256"
echo "[link-vk] driver archives, Mesa utility objects and glslang ready"

# 4. The pie link (twice: the second pass localizes everything but the imports), as conv.sh.
STUBS=$(find "$SDK/target/lib" -maxdepth 1 -name '*.so' ! -name 'libScePosixForWebKit.so' ! -name 'libSceGLSlimVSH.so' | sort)
pie_link() {
  # shellcheck disable=SC2086
  "$LLD" -m elf_x86_64 -pie -z max-page-size=0x4000 -mllvm -emulated-tls \
    --hash-style=gnu -T "$NATIVE/tooling/native/ps5-pie.ld" -T "$here/orbis-shims/ehframe.ld" --eh-frame-hdr \
    --version-script "$NATIVE/tooling/native/app-symbols.map" "$@" -e _start \
    ${LLD_EXTRA:---no-dynamic-linker} \
    -o "$OUT/build/llvm-pie.elf" $OBJS "${MESA_UTIL[@]}" \
    --whole-archive "${VK_ARCHIVES[@]}" --no-whole-archive \
    "${GLSLANG_LIBS[@]}" \
    --as-needed $STUBS \
    -L "$SDK"/target/lib \
    "$SDK"/target/lib/libc++.a "$SDK"/target/lib/libc++abi.a \
    "$SDK"/target/lib/libunwind.a "$SDK"/target/lib/libc.a -lpthread
}
pie_link
echo "[link-vk] pie link OK"
python3 "$port/genmap.py" "$OUT/build/llvm-pie.elf" "$SDK/target/lib" "$OUT/build/orbis-exports.map"
pie_link --version-script "$OUT/build/orbis-exports.map"
echo "[link-vk] pie link (localized) OK"

# 5. Native link, console preparation and signing, as conv.sh. The host tool is built from
# the boilerplate's sources (proto/native/tooling/native) the first time.
if [[ ! -x $TOOL ]]; then
  mkdir -p "$(dirname -- "$TOOL")"
  "${HOST_CXX:-clang++-18}" -std=c++20 -O2 -o "$TOOL" \
    "$NATIVE"/tooling/native/{native_app_builder,elf_object,self_container,sce_module_writer}.cpp -lz
  echo "[link-vk] built $TOOL"
fi
"$TOOL" link --in "$OUT/build/llvm-pie.elf" --out "$OUT/build/eboot.elf" \
  --stub-dir "$SDK/target/lib" --module-sdk 0x02000009 \
  --companion-sdk 0x08050001 --file-name eboot.elf
echo "[link-vk] native link OK"
python3 "$PREPARE" --input "$OUT/build/eboot.elf" --output "$OUT/build/eboot-console-ready.elf"
"$TOOL" self --sign --in "$OUT/build/eboot.elf" --out "$OUT/app/eboot.bin" --magic 0x1D3D154F
"$TOOL" self --inspect --file "$OUT/app/eboot.bin"
sha256sum "$OUT/app/eboot.bin" "$OUT/build/eboot-console-ready.elf"
echo "[link-vk] signed eboot OK ($TAG)"
