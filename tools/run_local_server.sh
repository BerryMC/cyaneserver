#!/usr/bin/env bash
# 启停本机测试用的 cyane 实例，用 pidfile 管理避免 pkill -f 自匹配。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${CYANE_TEST_CONFIG:-/tmp/cyane-test.toml}"
PIDFILE="${CYANE_TEST_PIDFILE:-/tmp/cyane-test.pid}"
LOGFILE="${CYANE_TEST_LOG:-/tmp/cyane-test.log}"

start() {
  if [[ -f "${PIDFILE}" ]] && kill -0 "$(cat "${PIDFILE}")" 2>/dev/null; then
    printf 'already running (pid %s)\n' "$(cat "${PIDFILE}")"
    return 0
  fi
  rm -f "${PIDFILE}"
  setsid "${ROOT}/build/cyane" --config "${CONFIG}" > "${LOGFILE}" 2>&1 < /dev/null &
  echo $! > "${PIDFILE}"
  sleep 1.5
  if kill -0 "$(cat "${PIDFILE}")" 2>/dev/null; then
    printf 'started (pid %s), log %s\n' "$(cat "${PIDFILE}")" "${LOGFILE}"
  else
    printf 'failed to start, see %s\n' "${LOGFILE}" >&2
    tail -5 "${LOGFILE}" >&2 || true
    return 1
  fi
}

stop() {
  if [[ -f "${PIDFILE}" ]]; then
    local pid
    pid="$(cat "${PIDFILE}")"
    if kill -0 "${pid}" 2>/dev/null; then
      kill -TERM "${pid}"
      for _ in $(seq 1 50); do
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.1
      done
      kill -KILL "${pid}" 2>/dev/null || true
      printf 'stopped (pid %s)\n' "${pid}"
    fi
    rm -f "${PIDFILE}"
  else
    printf 'not running\n'
  fi
}

case "${1:-}" in
  start) start ;;
  stop) stop ;;
  restart) stop; start ;;
  *) printf 'usage: %s {start|stop|restart}\n' "$0" >&2; exit 2 ;;
esac
