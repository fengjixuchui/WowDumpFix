#include "wow_imports.h"

#include <string>
#include <vector>

#include "fix_dump.h"
#include "memory.h"
#include "plugin.h"

static const SIZE_T iatMaxEntryCount = 4096;

static SIZE_T GetImportAddressTable(const REMOTE_PE_HEADER& HeaderData)
{
    PIMAGE_SECTION_HEADER rdata = GetPeSectionByName(HeaderData, ".rdata");
    return rdata != nullptr ? HeaderData.remoteBaseAddress + rdata->VirtualAddress : 0;
}

wow_imports::ImportUnpacker::~ImportUnpacker()
{
    if (hCapstone) {
        cs_close(&hCapstone);
        cs_free(insn, 1);
    }
}

bool wow_imports::ImportUnpacker::initialize()
{
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &hCapstone) != CS_ERR_OK)
        return false;

    if (cs_option(hCapstone, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
        return false;

    insn = cs_malloc(hCapstone);

    return true;
}

SIZE_T wow_imports::ImportUnpacker::resolve(SIZE_T ThunkBase)
{
    const SIZE_T blockSize = 0x50;
    SIZE_T import = 0;
    SIZE_T ea = ThunkBase;

    for (;;) {
        const SIZE_T readSize = min(blockSize, (ThunkBase & 0xFFFFFFFFFFFFF000) + PAGE_SIZE - ea);
        unsigned char codeBlock[blockSize] = { 0 };

        memset(codeBlock, 0x90, blockSize);

        if (!memory::util::RemoteRead(ea, codeBlock, readSize))
        {
            pluginLog("Error: failed to read 0x%llX bytes at %p.\n", readSize, ea);
            return 0;
        }

        if (resolveBlock(codeBlock, readSize, ea, import))
            break;
    }

    return import;
}

bool wow_imports::ImportUnpacker::resolveBlock(const unsigned char * CodeBuf, SIZE_T CodeSize, SIZE_T & EA, SIZE_T & Import)
{
    try
    {
        SIZE_T count = cs_disasm(hCapstone, CodeBuf, CodeSize, EA, 5, &insn);
        for (SIZE_T i = 0; i < count; i++)
        {
            cs_x86 *pX86 = NULL;
            if (insn[i].detail != NULL)
            {
                pX86 = &(insn[i].detail->x86);
            }
            else
            {
                return false;
            }

            uint8_t nIndex = pX86->op_count - 1;

            switch (insn[i].id)
            {
            case X86_INS_MOV:
            {
                Import = pX86->operands[nIndex].imm;
                break;
            }
            case X86_INS_MOVABS:
            {
                Import = pX86->operands[nIndex].imm;
                break;
            }
            case X86_INS_ADD:
            {
                Import += pX86->operands[nIndex].imm;
                break;
            }
            case X86_INS_SUB:
            {
                Import -= pX86->operands[nIndex].imm;
                break;
            }
            case X86_INS_XOR:
            {
                Import ^= pX86->operands[nIndex].imm;
                break;
            }
            // jmp rax = end of block, the import should be resolved.
            // jmp [IMMEDIATE] = continue resolving the thunk at a new block base, inside the current region.
            case X86_INS_JMP:
            {
                if (pX86->operands[nIndex].type == X86_OP_REG) {
                    return true;
                }
                else {
                    EA = pX86->operands[nIndex].imm;
                    return false;
                }
                break;
            }
            default:
            {
                pluginLog("Error: encountered unhandled instruction opcode while unpacking import at %p.\n", EA);
                EA = 0;
                return false;
            }
            }
        }
        return false;
    }
    catch (...)
    {
        pluginLog("resolveBlock Exception\n");
        return false;
    }
    return true;
}

bool wow_imports::RebuildImports(const REMOTE_PE_HEADER& HeaderData)
{
    ImportUnpacker unpacker;
    if (!unpacker.initialize()) {
        pluginLog("Error: failed to initialize import unpacker.\n");
        return false;
    }

    // import thunks to packed code blocks start at .rdata's base address.
    const SIZE_T importAddressTable = GetImportAddressTable(HeaderData);

    pluginLog("Found IAT at %llx.\n", importAddressTable);

    SIZE_T iatThunkArray[iatMaxEntryCount] = { 0 };

    if (!memory::util::RemoteRead(importAddressTable, PVOID(iatThunkArray),
                                  iatMaxEntryCount * sizeof(SIZE_T))) {
        pluginLog("Error: failed to read import address table at %p.\n",
                  importAddressTable);
        return false;
    }

    int importCountDelta = 1;

    // walk the table, resolving all thunks to their real va destination.
    std::vector<SIZE_T> unpackedThunkArray;
    for (int i = 0; iatThunkArray[i] > 0 && iatThunkArray[i] < debuggee.imageBase; i++) 
    {
        if (iatThunkArray[i] >= 0x00007FF000000000)
        {
            break;
        }

        for (/**/; iatThunkArray[i] > 0; i++)
        {
            if (iatThunkArray[i] >= debuggee.imageBase)
            {
                break;
            }

            SIZE_T resolveret = unpacker.resolve(iatThunkArray[i]);
            unpackedThunkArray.push_back(resolveret);
        }
        unpackedThunkArray.push_back(0);
        importCountDelta++;
    }
    
    unpackedThunkArray.push_back(0);

    const DWORD iatSize = DWORD(unpackedThunkArray.size() * sizeof(SIZE_T));

    // replace packed thunks with resolved virtual addresses.
    if (!memory::util::RemoteWrite(importAddressTable,
                                   PVOID(unpackedThunkArray.data()), iatSize)) {
        pluginLog("Error: failed to write unpacked thunk array to %p.\n", importAddressTable);
        return false;
    }

    // update the header's import address table pointer and size.
    const SIZE_T iatDDAddress = SIZE_T(HeaderData.dataDirectory[IMAGE_DIRECTORY_ENTRY_IAT]) -
                                SIZE_T(HeaderData.dosHeader) +
                                HeaderData.remoteBaseAddress;

    const DWORD iatRVA = DWORD(importAddressTable - HeaderData.remoteBaseAddress);

    if (!memory::util::RemoteWrite(iatDDAddress, PVOID(&iatRVA), sizeof(iatRVA)) ||
        !memory::util::RemoteWrite(iatDDAddress + sizeof(DWORD), PVOID(&iatSize), sizeof(iatSize))) {
        pluginLog("Error: failed to patch IAT data directory at %llx.\n", iatDDAddress);
        return false;
    }

    pluginLog("Restored %d imports at %llx.\n",
              unpackedThunkArray.size() - importCountDelta, importAddressTable);

    return true;
}
