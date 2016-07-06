#!/usr/bin/python3

import sys, subprocess

###############################################################################
# Config
###############################################################################

dataFile = None
binFile = None
onlyFunc = False
verbose = False

###############################################################################
# Utility functions
###############################################################################

def printHelp():
	print("stack-depth-info.py: parse stack depth data and summarize information\n")

	print("Usage: ./stack-depth-info.py -d file [ OPTIONS ]")
	print("Options:")
	print("  -h / --help : print help & exit")
	print("  -d file     : stack depth file dumped by library (usually stack_depth.dat)")
	print("  -b file     : binary from which data was dumped, gives more detailed information")
	print("  -f          : only print names of functions who called the stack depth library (requires -b)")
	print("  -v          : verbose output, prints caller information")

def parseData(fileName):
	numCalls = 0
	avgDepth = 0.0
	maxDepth = (0, 0, 0)
	funcCalls = []

	# Read in raw data, which is of the format:
	# (<function>, <# of calls>, <average depth>, (max depth caller, max depth), [<callers>])
	fp = open(fileName, 'r')
	for line in fp:
		tup = eval(line.strip())
		numCalls += tup[1]
		avgDepth += tup[1] * tup[2]
		if tup[3][1] > maxDepth[2]:
			maxDepth = (tup[0], tup[3][0], tup[3][1])
		funcCalls.append(tup)
	fp.close()

	avgDepth /= float(numCalls)
	return avgDepth, maxDepth, sorted(funcCalls, key=lambda func: func[1], reverse=True)

def printRaw(dataFile, avgDepth, maxDepth, funcCalls):
	global verbose

	print("Data from " + dataFile)
	print("Average depth: {:4.3f}".format(avgDepth))
	print("Max depth: " + str(maxDepth[2]) + ", " + hex(maxDepth[0]) + " called by " + hex(maxDepth[1]))
	print()
	print("{0:<14s} {1:>12s} {2:>12s}".format("Function:", "Num Calls", "Avg. Depth"))
	for val in funcCalls:
		print("0x{0:<12x} {1:>12d} {2:>12.3f}".format(val[0], val[1], val[2]))

	if verbose:
		for val in funcCalls:
			print("\n\r" + hex(val[0]) + " called by:")
			callers = sorted(val[4], key=lambda func: func[1], reverse=True)
			for caller in callers:
				print("  0x{0:<x}: {1:d} time(s)".format(caller[0], caller[1]))

def getSymbolTable(binFile):
	symbols = {}
	out = subprocess.check_output(["readelf", "--symbols", binFile])
	outlines = out.decode("utf-8").split("\n")
	for line in outlines:
		if "Symbol table" in line or "Num:" in line:
			continue
		else:
			toks = line.strip().split()
			if len(toks) < 8:
				continue
			startAddr = int(toks[1], base=16)
			if startAddr == 0: # Skip undefined symbols
				continue
			if "x" in toks[2]:
				size = int(toks[2], base=16)
			else:
				size = int(toks[2])
			if size == 0: # For dynamically loaded symbols
				size = 1
			name = toks[7].split("@")[0]
			symbols[name] = (startAddr, size)
	return symbols

def printDetailed(dataFile, binFile, symbols, avgDepth, maxDepth, funcCalls):
	global onlyFunc
	global verbose

	def getSymbol(symbols, addr):
		for sym in symbols:
			endAddr = symbols[sym][0] + symbols[sym][1]
			if symbols[sym][0] <= addr and addr < endAddr:
				return sym
		return "(n/a)"

	if onlyFunc:
		for val in funcCalls:
			sym = getSymbol(symbols, val[0])
			print(sym)
	else:
		print("Data from " + dataFile + ", generated by " + binFile)
		print("Average depth: {:4.3f}".format(avgDepth))
		print("Max depth: " + str(maxDepth[2]) + ", " + \
			getSymbol(symbols, maxDepth[0]) + " (" + hex(maxDepth[0]) + ") called by " + \
			getSymbol(symbols, maxDepth[1]) + " (" + hex(maxDepth[1]) + ")")
		print()
		print("{0:<55s} {1:>12s} {2:>12s}".format("Function:", "Num Calls", "Avg. Depth"))
		for val in funcCalls:
			sym = getSymbol(symbols, val[0])
			print("{0:<55s} {1:>12d} {2:>12.3f}".format(sym + " (" + hex(val[0]) + ")", val[1], val[2]))

		if verbose:
			for val in funcCalls:
				print("\n\r" + getSymbol(symbols, val[0]) + " called by:")
				callers = sorted(val[4], key=lambda func: func[1], reverse=True)
				for caller in callers:
					sym = getSymbol(symbols, caller[0])
					print("  {0:s}: {1:d} time(s)".format(sym + " (" + hex(caller[0]) + ")", caller[1]))

###############################################################################
# Driver
###############################################################################

skip = False
for i in range(len(sys.argv)):
	if skip:
		skip = False
		continue
	elif sys.argv[i] == "-h" or sys.argv[i] == "--help":
		printHelp()
		sys.exit(0)
		continue
	elif sys.argv[i] == "-d":
		dataFile = sys.argv[i+1]
		skip = True
		continue
	elif sys.argv[i] == "-b":
		binFile = sys.argv[i+1]
		skip = True
		continue
	elif sys.argv[i] == "-f":
		onlyFunc = True
		continue
	elif sys.argv[i] == "-v":
		verbose = True
		continue

if dataFile == None:
	print("Please supply a data file!")
	printHelp()
	sys.exit(1)

if onlyFunc and binFile == None:
	print("Please supply a binary to print function names!")
	printHelp()
	sys.exit(1)

avgDepth, maxDepth, funcCalls = parseData(dataFile)
if binFile == None:
	printRaw(dataFile, avgDepth, maxDepth, funcCalls)
else:
	symbols = getSymbolTable(binFile)
	printDetailed(dataFile, binFile, symbols, avgDepth, maxDepth, funcCalls)
