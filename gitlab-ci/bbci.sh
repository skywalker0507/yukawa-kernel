#!/bin/sh

set -xe

BBCI_PATH=$1
SOURCE_NAME=$2
TARGET_NAME=$3
BOARD_TAG=$4

BBCI_OPTS="-d --nolog -s $SOURCE_NAME -t $TARGET_NAME -D $BOARD_TAG"

cd $BBCI_PATH

#echo "Cloning Linux..."
#./bbci.py $BBCI_OPTS -a create

echo "Downloading Toolchain"
./bbci.py $BBCI_OPTS -a download

echo "Building kernel"
./bbci.py $BBCI_OPTS --configoverlay vanilla --noclean -a build

echo "Generating LAVA test jobs"
./bbci.py $BBCI_OPTS --configoverlay vanilla --noact -a boot

exit 0
