#!/usr/bin/env bash
# sc —— StackChan 木偶固件命令行 (调用本机 stackchan-host :5310)
set -euo pipefail
BASE="${STACKCHAN_URL:-http://127.0.0.1:5310}"

post(){ curl -s --noproxy '*' -XPOST -H 'Content-Type: application/json' "$BASE/$1" -d "$2"; echo; }
js(){ python3 -c 'import json,sys;print(json.dumps(sys.argv[1],ensure_ascii=False))' "$1"; }

case "${1:-}" in
  say)      shift; post say "{\"text\":$(js "$*")}" ;;
  bubble)   shift; post bubble "{\"text\":$(js "$*")}" ;;
  text)     shift; post show "{\"text\":$(js "$*"),\"ms\":${SC_MS:-0}}" ;;
  show)     src="$2"; key=path; [[ "$src" == http* ]] && key=url
            post show "{\"$key\":$(js "$src"),\"ms\":${3:-0}}" ;;
  play)     post play "{\"path\":$(js "$2")}" ;;
  head)     post head "{\"yaw\":${2:-0},\"pitch\":${3:-45},\"ms\":${4:-400}}" ;;
  gesture|g) post gesture "{\"name\":\"$2\"}" ;;
  face)     post face "{\"expr\":\"${2:-neutral}\"}" ;;
  clear)    post clear '{}' ;;
  stop)     post stop '{}' ;;
  volume)   post volume "{\"v\":$2}" ;;
  status)   curl -s --noproxy '*' "$BASE/status" | python3 -m json.tool --no-ensure-ascii ;;
  events)   curl -sN --noproxy '*' "$BASE/events" ;;
  *) cat <<'USAGE'
用法:
  sc say <文字>            说话 (Mac 上 TTS)
  sc bubble <文字>         表情脸上显示气泡文字
  sc text <文字>           整屏大字 (SC_MS=5000 sc text ... 显示 5 秒后回到表情)
  sc show <路径|URL> [ms]  显示图片, ms>0 时到点自动回到表情
  sc play <音频文件>       播放本地音频 (mp3/wav/m4a...)
  sc head <yaw> <pitch> [ms]   转头: yaw -45~45(正=向左), pitch 30~60(大=抬头)
  sc g <nod|shake|dizzy|look_around|tilt>   预设动作
  sc face <happy|angry|sad|doubt|sleepy|neutral>
  sc clear | stop | volume <0-255> | status | events(实时看摸头等事件)
USAGE
  ;;
esac
