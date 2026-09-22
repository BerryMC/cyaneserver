#!/usr/bin/env bash
# 离线物化 Spigot 1.12.2 服务端 jar：vanilla/server.jar + spigotMC.patch。
# jars/ 不入库、不分发，仅本机用于签名提取与行为对照。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JARS="${ROOT}/jars"
VANILLA="${JARS}/vanilla/server.jar"
BOOTSTRAP="${JARS}/spigot/server.jar"
PATCH="${JARS}/spigot/spigotMC.patch"
OUTPUT="${JARS}/spigot/spigot-1.12.2.jar"

VANILLA_SHA="fe1f9274e6dad9191bf6e6e8e36ee6ebc737f373603df0946aafcded0d53167e"
PATCHED_SHA="ff5440e15f371b6def688c86bce296f8451fa8d00df9ad4270eb250621a468f2"

fail() { printf 'reproduce_jars: %s\n' "$1" >&2; exit 1; }
digest() { sha256sum "$1" | cut -d' ' -f1; }

[[ -f "${VANILLA}" ]] || fail "缺少 ${VANILLA}"
[[ -f "${BOOTSTRAP}" ]] || fail "缺少 ${BOOTSTRAP}（Paperclip 引导壳）"

actual="$(digest "${VANILLA}")"
[[ "${actual}" == "${VANILLA_SHA}" ]] || fail "vanilla 哈希不符，期望 ${VANILLA_SHA}，实际 ${actual}"
printf '==> vanilla 校验通过\n'

if [[ -f "${OUTPUT}" ]] && [[ "$(digest "${OUTPUT}")" == "${PATCHED_SHA}" ]]; then
  printf '==> 已存在且哈希正确：%s\n' "${OUTPUT}"
  exit 0
fi

printf '==> 提取 spigotMC.patch\n'
unzip -o -q "${BOOTSTRAP}" spigotMC.patch -d "${JARS}/spigot"
[[ -f "${PATCH}" ]] || fail "补丁提取失败"

printf '==> 应用 jbsdiff 补丁（约 20s）\n'
java -cp "${BOOTSTRAP}" org.jbsdiff.ui.CLI patch "${VANILLA}" "${OUTPUT}" "${PATCH}"

actual="$(digest "${OUTPUT}")"
[[ "${actual}" == "${PATCHED_SHA}" ]] || fail "派生 jar 哈希不符，期望 ${PATCHED_SHA}，实际 ${actual}"
printf '==> 完成：%s (%s bytes)\n' "${OUTPUT}" "$(stat -c %s "${OUTPUT}")"
