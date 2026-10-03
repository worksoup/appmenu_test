#!/bin/sh
# Build appmenu_test against both Qt 6 and Qt 5 (whichever is installed).
set -e
cd "$(dirname "$0")"

FLAGS="-std=c++17 -fPIC -Wall"

if pkg-config --exists Qt6Widgets; then
    g++ $FLAGS appmenu_test.cpp -o appmenu_test-qt6 \
        $(pkg-config --cflags --libs Qt6Widgets Qt6OpenGL)
    echo "built: $(pwd)/appmenu_test-qt6  (Qt $(pkg-config --modversion Qt6Widgets))"
fi

if pkg-config --exists Qt5Widgets; then
    g++ $FLAGS appmenu_test.cpp -o appmenu_test-qt5 \
        $(pkg-config --cflags --libs Qt5Widgets Qt5OpenGL)
    echo "built: $(pwd)/appmenu_test-qt5  (Qt $(pkg-config --modversion Qt5Widgets))"
fi
