#!/bin/bash

set -euo pipefail

export EMBEDDED_MODEL=./rife-ncnn-vulkan/models/rife-v4.26/

LOW_PRIO="/usr/bin/chrt --idle 0 /usr/bin/nice -n19 /usr/bin/ionice --class idle"

SUBDIR="rife-ncnn-vulkan/src"
[ -d "${SUBDIR}" ] || git submodule update --init --recursive --depth 0 -j 3

cp "./rife_transition.cpp" "${SUBDIR}/rife_transition.cpp"
sed -i "s/@@EMBEDDED_MODEL_NAME@@/$(basename $EMBEDDED_MODEL)/g" "${SUBDIR}/rife_transition.cpp"

ln -s --force "${PWD}/${EMBEDDED_MODEL}/flownet.bin" "${SUBDIR}/flownet.bin"
ln -s --force "${PWD}/${EMBEDDED_MODEL}/flownet.param" "${SUBDIR}/flownet.param"

(cd "${SUBDIR}" && git checkout CMakeLists.txt)
sed -i '/target_link_libraries/d' "${SUBDIR}/CMakeLists.txt"
sed -i '/add_executable(rife-ncnn-vulkan/,/)/d' "${SUBDIR}/CMakeLists.txt"
sed -i '/add_dependencies(rife-ncnn-vulkan/,/)/d' "${SUBDIR}/CMakeLists.txt"
cat << EOF >> "${SUBDIR}/CMakeLists.txt"
  add_custom_command(
      OUTPUT flownetbin.o
      COMMAND ld -z noexecstack -r -b binary flownet.bin -o \${CMAKE_BINARY_DIR}/flownetbin.o -m elf_x86_64
      DEPENDS flownet.bin
      WORKING_DIRECTORY \${CMAKE_CURRENT_SOURCE_DIR})
  add_custom_command(
      OUTPUT flownetparam.o
      COMMAND ld -z noexecstack -r -b binary flownet.param -o \${CMAKE_BINARY_DIR}/flownetparam.o -m elf_x86_64
      DEPENDS flownet.param
      WORKING_DIRECTORY \${CMAKE_CURRENT_SOURCE_DIR})

  add_custom_target(
      generate-embedded-flownet
      DEPENDS \${CMAKE_BINARY_DIR}/flownetbin.o \${CMAKE_BINARY_DIR}/flownetparam.o
  )

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  add_library (rife_transition MODULE rife_transition.cpp rife.cpp warp.cpp flownetbin.o flownetparam.o)
  add_dependencies(rife_transition generate-spirv generate-embedded-flownet)
  target_link_libraries(rife_transition ncnn \${Vulkan_LIBRARY})
EOF

inputs=("${0}" "rife_transition.cpp" "${PWD}/${EMBEDDED_MODEL}/flownet.bin" "${PWD}/${EMBEDDED_MODEL}/flownet.param")
newest_mtime=0
for file in "${inputs[@]}"; do
  mtime=$(stat -c %Y "$file")
  if [ "$newest_mtime" -lt "$mtime" ]; then newest_mtime="$mtime"; fi
done
output=$(stat -c %Y "rife_transition.so" || echo "0")

if [ "$output" -lt "$newest_mtime" ]; then
  mtime_formatted=$(date -d "@$newest_mtime" +'%Y-%m-%d %H:%M:%S')
  (cd "${SUBDIR}/" && touch --no-dereference -d "${mtime_formatted}" rife_transition.cpp CMakeLists.txt flownet.bin flownet.param)

  mkdir -p build
  cd build/

  ${LOW_PRIO} cmake -DUSE_SYSTEM_WEBP=ON "../${SUBDIR}"
  ${LOW_PRIO} cmake --build . -j

  mv librife_transition.so ../rife_transition.so
fi

echo "Plugin built and saved as rife_transition.so (embedded ${EMBEDDED_MODEL})"
