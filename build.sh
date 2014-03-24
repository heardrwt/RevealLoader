#!/bin/bash
make clean
make
make package
make install THEOS_DEVICE_IP=rPad.local

