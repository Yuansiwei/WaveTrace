"""
Script to generate $cmodel_path/inc/gcDefine.h for both DV and emulator env
Input is $cmodel_path/CModelFeature.xlsx and argv.arch
"""

featureDB = "CModelFeatureDB.csv"

import os
import re
import sys
import datetime
import csv
import io
import stat


# We assume workbook must be organized by following format
# archName  Description         $arch1     $arch2 ...
# $defName1 $def1Description    $arch1Def1 $arch2Def1 ...
# $defName2 $def2Description    $arch1Def2 $arch2Def2 ...
# ...


def PrintHelp():
    print('\nDecscription:')
    print('\t {} is used to generate gcDefines.h for cmodel.\n'.format(os.path.basename(__file__)))
    print('Usage:')
    print('\t {} cmodel_path arch [useHack]'.format(os.path.basename(__file__)))
    print('\t $cmodel_path/inc/gcDefine.h will be generated according to macros defined under $arch in $cmodel_path/CModelFeature.xlsx.\n')
    print('\t So, cmodel_path and arch must be correctly specified.\n')
    print('\t useHack is 0 or 1 for whether to hack some features generated in gcDefines.h, $cmodel_path/inc/HackGcDefine/$arch.txt will be used.\n')


def PrintDef2File(fileHandler, defName, defValue, arch, isChipInfo):
    macro = defName
    if (defValue is None):
        print('[gcDefineGen] ERROR: {} has no value set for {}.'.format(defName, arch))
        return
    
    isBoolValue = ('true' == defValue.lower()) or ('false' == defValue.lower())
    defName = 'gcd{}'.format(defName) if isBoolValue else 'gcv{}'.format(defName)
    
    if isBoolValue:
        defValue = 1 if ('true' == defValue.lower()) else 0
        
    if useHack:
        defValue = hackD.pop(macro, defValue)
        
    fileHandler.write('#define {} {}\n'.format(defName, defValue) if isChipInfo else 
'''\n#ifndef {}
#define {} {}
#endif\n'''.format(defName, defName, defValue))
    

def GenDefineHeader(fileHandler, arch):
    fileHandler.write(
'''// This file is generated automatically by {}
// do NOT change it manually!!!
// Chip --{}-- generated at {} CST\n\n'''.format(__file__, arch, datetime.datetime.now().strftime('%H:%M:%S, %B %d, %Y')))
    
    if useHack:
        fileHandler.write('// !!!!!! GENERATED using {} for HACKING !!!!!!\n\n'.format(HackDBFile))

def NormalizeGeneratedHeader(text):
    return re.sub(r'// Chip --([^\n]*)-- generated at .* CST', r'// Chip --\1-- generated at <timestamp> CST', text)


def WriteIfChanged(path, content):
    old = None
    try:
        with open(path, 'r') as f:
            old = f.read()
    except FileNotFoundError:
        old = None

    if old is not None and NormalizeGeneratedHeader(old) == NormalizeGeneratedHeader(content):
        print('[gcDefineGen] INFO: {} unchanged.'.format(path))
        return False

    tmp = '{}.tmp'.format(path)
    try:
        if os.path.exists(path):
            os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
    except Exception:
        pass

    with open(tmp, 'w') as f:
        f.write(content)
    os.replace(tmp, path)
    print('[gcDefineGen] INFO: {} updated.'.format(path))
    return True


def GenDefineHppFromCsv(cmodel_path, arch):
    csvf = open(os.path.join(cmodel_path, featureDB))
    reader = csv.DictReader(csvf)

    gcDefineGen = os.path.join(cmodel_path, 'inc', 'gcDefines.h')
    gcDefineFileHandler = io.StringIO()
    GenDefineHeader(gcDefineFileHandler, arch)
 
    isChipInfo = True
    archNotFound = False
    for row in reader:
        if row['MacroType'] == 'Feature':
            isChipInfo = False

        if arch in row.keys():
            PrintDef2File(gcDefineFileHandler, row['MacroName'], row[arch], arch, isChipInfo)
        else:
            archNotFound = True
            break
    
    csvf.close()

    if archNotFound:
        print("[gcDefineGen] ERROR: given arch {} not founded in {}.".format(arch, os.path.join(cmodel_path, featureDB)))
        if os.path.exists(gcDefineGen):
            os.remove(gcDefineGen)
        return

    WriteIfChanged(gcDefineGen, gcDefineFileHandler.getvalue())

useHack = 0
hackD = dict()

if "__main__" == __name__:
    argus = sys.argv[1:]
    if len(argus) < 2:
        PrintHelp()
        exit(0)
        
    cmodel_path, arch = argus[0:2]
    
    if len(argus) > 2:
        useHack = eval(argus[2])

    DBFile = os.path.join(cmodel_path, featureDB)
    HackDBFile = os.path.join(cmodel_path, "inc", "HackGcDefine", "{}.txt".format(arch))

    if not os.path.exists(DBFile):
        PrintHelp()
        print('[gcDefineGen] ERROR: cmodel_path "{}" not correct or there is no {} under cmodel_path.'.format(cmodel_path, featureDB))
        exit(0)
        
    if useHack:
        try:
            with open(HackDBFile) as f:
                for line in f:
                    pair = line.split()
                    hackD[pair[0]] = pair[1]
            print("[gcDefineGen] INFO: hack gcDefines.h using {}.".format(HackDBFile))
        except FileNotFoundError:
            print("[gcDefineGen] WARNING: no gcDefine hack used since {} not found.".format(HackDBFile))

    GenDefineHppFromCsv(cmodel_path, arch)
