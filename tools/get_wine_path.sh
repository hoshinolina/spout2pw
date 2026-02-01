#!/bin/sh
set -e

cd "$MESON_BUILD_ROOT"

cat > "winegcc_test.c" <<EOF
int main() { return 0; }
EOF

target="$1"
grep="$2"

winegcc -v ${target:+-b}$target -o winegcc_test.exe winegcc_test.c &>winegcc_output.txt
cat winegcc_output.txt | sed 's/ /\n/g' | grep "$grep"
