#!/bin/sh
# Drives /disk/bastio-lsp over standard input and output the way an editor
# does, and prints "ok WHAT" if the reply holds the fact bastio.txt expects.
# Only the shell is used: the Bastio disk carries no other tools.
#
#   lsp_session.sh WHAT      WHAT: hover | definition | format | diagnostic
m() { printf 'Content-Length: %s\r\n\r\n%s' "${#1}" "$1"; }
init='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"initializationOptions":{"bastioc":"/disk/bastioc1"}}}'
tail='{"jsonrpc":"2.0","id":99,"method":"shutdown"}'
bye='{"jsonrpc":"2.0","method":"exit"}'
open_doc() { # URI TEXT (JSON-escaped)
  printf '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"%s","languageId":"bastio","version":1,"text":"%s"}}}' "$1" "$2"
}
case $1 in
hover | definition)
  text=
  while IFS= read -r l; do text="$text$l\\n"; done < /disk/lsp_demo.bastio
  u=file:///disk/lsp_demo.bastio
  # The call of `double` is on line 8 (counting from 0), after "  let n = ".
  req="{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/$1\",\"params\":{\"textDocument\":{\"uri\":\"$u\"},\"position\":{\"line\":8,\"character\":11}}}"
  out=$({ m "$init"; m "$(open_doc $u "$text")"; m "$req"; m "$tail"; m "$bye"; } | /disk/bastio-lsp)
  case $1,$out in
  hover,*'fn double(x: u64) -> u64\n  requires x < 1000'*'Doubles `x`.'*) echo ok hover ;;
  definition,*'"range":{"start":{"line":1,"character":3}'*) echo ok definition ;;
  *) echo "FAIL $1: $out" ;;
  esac
  ;;
format)
  u=file:///x/ugly.bastio
  req="{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/formatting\",\"params\":{\"textDocument\":{\"uri\":\"$u\"},\"options\":{\"tabSize\":2,\"insertSpaces\":true}}}"
  out=$({ m "$init"; m "$(open_doc $u 'fn main()->u64{ return 1 ; }\n')"; m "$req"; m "$tail"; m "$bye"; } | /disk/bastio-lsp)
  case $out in
  *'"newText":"fn main() -> u64 { return 1; }\n"'*) echo ok format ;;
  *) echo "FAIL format: $out" ;;
  esac
  ;;
diagnostic)
  d=/cb_lsp
  mkdir -p $d && printf 'fn main() -> u64 {\n  return true;\n}\n' > $d/t.bastio
  u=file://$d/t.bastio
  out=$({ m "$init"; m "$(open_doc $u 'fn main() -> u64 {\n  return true;\n}\n')"; m "$tail"; m "$bye"; } | /disk/bastio-lsp)
  rm -rf $d
  case $out in
  *'"range":{"start":{"line":1,"character":2}'*'"code":"E-TYPE"'*) echo ok diagnostic ;;
  *) echo "FAIL diagnostic: $out" ;;
  esac
  ;;
esac
