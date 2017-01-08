LINUX_PATH = $(KERNEL_DIR)
GCC = arm-linux-gnueabihf-gcc
INIPARSER = iniParser/iniparser.c iniParser/dictionary.c 

all:
	$(GCC) -I $(LINUX_PATH)/include -Wall -shared -ldl -fPIC -std=c99 -O3 -pthread -o screenInv.so screenInv.c $(INIPARSER)

debug:
	$(GCC) -I $(LINUX_PATH)/include -Wall -shared -ldl -fPIC -std=c99 -O3 -pthread -D SI_DEBUG -o screenInv.so screenInv.c $(INIPARSER)
