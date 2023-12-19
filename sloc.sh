#!/usr/bin/env bash
wc -l $(git ls-files '*.cpp' '*.h' '*.'c | grep -vF binfont.c | grep -v "^im" | grep -v "^stb_" )
