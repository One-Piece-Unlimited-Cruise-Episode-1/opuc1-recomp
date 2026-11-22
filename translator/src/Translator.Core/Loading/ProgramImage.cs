namespace Translator.Core.Loading;

public sealed class ProgramImage
{
    public ProgramImage(
        byte[] memory,
        AddressRange usedRange,
        AddressRange dolRange,
        string sha256,
        uint memoryBase = MemoryLayout.RamBase)
    {
        Memory = memory;
        UsedRange = usedRange;
        DolRange = dolRange;
        Sha256 = sha256;
        MemoryBase = memoryBase;
    }

    public byte[] Memory { get; }
    public uint MemoryBase { get; }
    public uint MemoryEnd => checked(MemoryBase + (uint)Memory.Length);
    public AddressRange UsedRange { get; }
    public AddressRange DolRange { get; }
    public string Sha256 { get; }

    public bool Contains(uint address, int size = 1)
    {
        if (size < 0 || address < MemoryBase)
        {
            return false;
        }

        var offset = (ulong)address - MemoryBase;
        return offset + (uint)size <= (ulong)Memory.Length;
    }

    public int GetOffset(uint address, int size = 1)
    {
        if (!Contains(address, size))
        {
            throw new ArgumentOutOfRangeException(
                nameof(address),
                $"Guest range 0x{address:X8}+0x{size:X} is outside 0x{MemoryBase:X8}-0x{MemoryEnd:X8}.");
        }

        return checked((int)(address - MemoryBase));
    }
}
