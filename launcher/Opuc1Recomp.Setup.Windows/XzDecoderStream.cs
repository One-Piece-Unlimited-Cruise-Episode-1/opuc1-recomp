using System.Buffers.Binary;

namespace Opuc1Recomp.Setup.Windows;

/// <summary>
/// A read-only, forward-only decoder for one .xz stream (XZ Utils file format 1.2): blocks whose filter chain
/// is LZMA2, optionally preceded by the x86 BCJ filter, with a CRC32, CRC64 or no integrity check. That is
/// every form <c>xz [--x86] --lzma2=...</c> writes, which is how Build-Installer.ps1 packs the payload. Each
/// block's check and the stream index are verified, so a damaged download fails here instead of extracting
/// silently wrong files.
/// </summary>
internal sealed class XzDecoderStream : Stream
{
    private const int MaxDictionarySize = 1536 << 20;
    private const int MaxChunkUnpacked = 2 << 20;
    private static readonly byte[] HeaderMagic = [0xFD, (byte)'7', (byte)'z', (byte)'X', (byte)'Z', 0x00];

    private readonly Stream _input;
    private readonly bool _leaveOpen;
    private int _checkType;
    private int _checkSize;
    private ushort _streamFlags;
    private readonly List<(long Unpadded, long Uncompressed)> _records = [];

    // Per block.
    private Lzma2Decoder? _lzma2;
    private BcjX86Filter? _bcj;
    private bool _inBlock;
    private long _blockHeaderSize;
    private long _blockCompressedStart;
    private long _blockUncompressed;
    private long? _declaredCompressed;
    private long? _declaredUncompressed;
    private Crc32 _crc32;
    private Crc64 _crc64;

    // Bytes the filter chain produced but the caller has not read yet.
    private byte[] _pending = new byte[64 * 1024];
    private int _pendingStart;
    private int _pendingEnd;
    private bool _finished;
    private long _inputPosition;

    public XzDecoderStream(Stream input, bool leaveOpen = false)
    {
        _input = input;
        _leaveOpen = leaveOpen;
        ReadStreamHeader();
    }

    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));

    public override int Read(Span<byte> destination)
    {
        if (destination.IsEmpty) return 0;
        while (_pendingStart == _pendingEnd)
        {
            if (_finished) return 0;
            Produce();
        }
        var count = Math.Min(destination.Length, _pendingEnd - _pendingStart);
        _pending.AsSpan(_pendingStart, count).CopyTo(destination);
        _pendingStart += count;
        return count;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && !_leaveOpen) _input.Dispose();
        base.Dispose(disposing);
    }

    /// <summary>Fills <see cref="_pending"/> with the next decoded bytes, or finishes the stream.</summary>
    private void Produce()
    {
        _pendingStart = _pendingEnd = 0;
        if (!_inBlock)
        {
            if (!BeginBlockOrIndex())
            {
                _finished = true;
                return;
            }
        }

        var produced = _lzma2!.DecodeNextChunk(this, out var chunk);
        if (produced == 0)
        {
            // The LZMA2 end marker: whatever the BCJ filter still holds back is final.
            if (_bcj is not null) AppendFiltered(_bcj.Flush());
            EndBlock();
            return;
        }
        if (_bcj is null) AppendFiltered(chunk);
        else AppendFiltered(_bcj.Filter(chunk));
    }

    private void AppendFiltered(ReadOnlySpan<byte> data)
    {
        if (data.IsEmpty) return;
        if (_pending.Length - _pendingEnd < data.Length)
        {
            var grown = new byte[Math.Max(_pending.Length * 2, _pendingEnd + data.Length)];
            _pending.AsSpan(0, _pendingEnd).CopyTo(grown);
            _pending = grown;
        }
        data.CopyTo(_pending.AsSpan(_pendingEnd));
        _pendingEnd += data.Length;
        _blockUncompressed += data.Length;
        if (_checkType == 1) _crc32.Append(data);
        else if (_checkType == 4) _crc64.Append(data);
    }

    // --- container -------------------------------------------------------------------------------------------

    internal int ReadByteOrThrow()
    {
        var value = _input.ReadByte();
        if (value < 0) throw Corrupt("the stream ends early");
        _inputPosition++;
        return value;
    }

    internal void ReadInput(Span<byte> buffer)
    {
        try { _input.ReadExactly(buffer); }
        catch (EndOfStreamException) { throw Corrupt("the stream ends early"); }
        _inputPosition += buffer.Length;
    }

    private void ReadStreamHeader()
    {
        Span<byte> header = stackalloc byte[12];
        ReadInput(header);
        if (!header[..6].SequenceEqual(HeaderMagic)) throw Corrupt("it is not an xz stream");
        if (Crc32.Compute(header.Slice(6, 2)) != BinaryPrimitives.ReadUInt32LittleEndian(header[8..]))
            throw Corrupt("the stream header checksum does not match");
        if (header[6] != 0 || (header[7] & 0xF0) != 0) throw Corrupt("the stream flags are not supported");
        _streamFlags = BinaryPrimitives.ReadUInt16LittleEndian(header.Slice(6, 2));
        _checkType = header[7] & 0x0F;
        _checkSize = _checkType switch
        {
            0 => 0,
            1 => 4,
            4 => 8,
            _ => throw Corrupt($"integrity check type {_checkType} is not supported")
        };
    }

    /// <summary>Reads a block header and starts the block; returns false after reading the index and footer.</summary>
    private bool BeginBlockOrIndex()
    {
        var blockStart = _inputPosition;
        var sizeByte = ReadByteOrThrow();
        if (sizeByte == 0)
        {
            ReadIndexAndFooter(blockStart);
            return false;
        }

        var headerSize = (sizeByte + 1) * 4;
        var header = new byte[headerSize];
        header[0] = (byte)sizeByte;
        ReadInput(header.AsSpan(1));
        if (Crc32.Compute(header.AsSpan(0, headerSize - 4)) !=
            BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(headerSize - 4)))
            throw Corrupt("a block header checksum does not match");

        var flags = header[1];
        if ((flags & 0x3C) != 0) throw Corrupt("a block header uses reserved flags");
        var filterCount = (flags & 0x03) + 1;
        var position = 2;
        _declaredCompressed = (flags & 0x40) != 0 ? (long)ReadVli(header, ref position) : null;
        _declaredUncompressed = (flags & 0x80) != 0 ? (long)ReadVli(header, ref position) : null;

        var filters = new List<(ulong Id, byte[] Properties)>();
        for (var i = 0; i < filterCount; i++)
        {
            var id = ReadVli(header, ref position);
            var propertiesSize = (int)ReadVli(header, ref position);
            if (position + propertiesSize > headerSize - 4) throw Corrupt("a filter description is truncated");
            filters.Add((id, header.AsSpan(position, propertiesSize).ToArray()));
            position += propertiesSize;
        }
        for (; position < headerSize - 4; position++)
            if (header[position] != 0) throw Corrupt("block header padding is not zero");

        var last = filters[^1];
        if (last.Id != 0x21 || last.Properties.Length != 1) throw Corrupt("the last filter is not LZMA2");
        _bcj = null;
        if (filters.Count == 2)
        {
            var first = filters[0];
            if (first.Id != 0x04) throw Corrupt($"filter 0x{first.Id:X} is not supported");
            uint startOffset = first.Properties.Length switch
            {
                0 => 0,
                4 => BinaryPrimitives.ReadUInt32LittleEndian(first.Properties),
                _ => throw Corrupt("the x86 filter properties are invalid")
            };
            _bcj = new BcjX86Filter(startOffset);
        }
        else if (filters.Count != 1) throw Corrupt("filter chains longer than x86 + LZMA2 are not supported");

        var dictionarySize = Lzma2Decoder.DictionarySizeFromProperty(last.Properties[0]);
        if (dictionarySize > MaxDictionarySize) throw Corrupt("its dictionary is larger than this setup supports");
        // One decoder per stream: blocks of one stream share the dictionary size, so the buffer is reused.
        if (_lzma2 is null || _lzma2.DictionarySize < dictionarySize)
            _lzma2 = new Lzma2Decoder(dictionarySize, MaxChunkUnpacked);
        _lzma2.StartBlock();

        _inBlock = true;
        _blockHeaderSize = headerSize;
        _blockCompressedStart = _inputPosition;
        _blockUncompressed = 0;
        _crc32 = new Crc32();
        _crc64 = new Crc64();
        return true;
    }

    private void EndBlock()
    {
        var compressed = _inputPosition - _blockCompressedStart;
        if (_declaredCompressed is { } declaredCompressed && declaredCompressed != compressed)
            throw Corrupt("a block's compressed size does not match its header");
        if (_declaredUncompressed is { } declaredUncompressed && declaredUncompressed != _blockUncompressed)
            throw Corrupt("a block's uncompressed size does not match its header");
        while ((_inputPosition & 3) != 0)
            if (ReadByteOrThrow() != 0) throw Corrupt("block padding is not zero");

        Span<byte> check = stackalloc byte[8];
        ReadInput(check[.._checkSize]);
        var valid = _checkType switch
        {
            1 => BinaryPrimitives.ReadUInt32LittleEndian(check) == _crc32.Value,
            4 => BinaryPrimitives.ReadUInt64LittleEndian(check) == _crc64.Value,
            _ => true
        };
        if (!valid) throw Corrupt("a block's integrity check does not match; the file is damaged");

        _records.Add((_blockHeaderSize + compressed + _checkSize, _blockUncompressed));
        _inBlock = false;
    }

    private void ReadIndexAndFooter(long indexStart)
    {
        var crc = new Crc32();
        crc.Append([0]);
        ulong ReadIndexVli()
        {
            ulong value = 0;
            for (var shift = 0; shift < 63; shift += 7)
            {
                var b = (byte)ReadByteOrThrow();
                crc.Append([b]);
                value |= (ulong)(b & 0x7F) << shift;
                if ((b & 0x80) == 0)
                {
                    if (b == 0 && shift != 0) throw Corrupt("the index has a non-minimal number");
                    return value;
                }
            }
            throw Corrupt("the index has an oversized number");
        }

        var count = ReadIndexVli();
        if (count != (ulong)_records.Count) throw Corrupt("the index lists a different number of blocks");
        foreach (var record in _records)
        {
            if (ReadIndexVli() != (ulong)record.Unpadded || ReadIndexVli() != (ulong)record.Uncompressed)
                throw Corrupt("the index does not describe the blocks that were read");
        }
        while (((_inputPosition - indexStart) & 3) != 0)
        {
            var b = (byte)ReadByteOrThrow();
            if (b != 0) throw Corrupt("index padding is not zero");
            crc.Append([b]);
        }
        Span<byte> stored = stackalloc byte[4];
        ReadInput(stored);
        if (BinaryPrimitives.ReadUInt32LittleEndian(stored) != crc.Value) throw Corrupt("the index checksum does not match");
        var indexSize = _inputPosition - indexStart;

        Span<byte> footer = stackalloc byte[12];
        ReadInput(footer);
        if (Crc32.Compute(footer.Slice(4, 6)) != BinaryPrimitives.ReadUInt32LittleEndian(footer))
            throw Corrupt("the stream footer checksum does not match");
        var backwardSize = ((long)BinaryPrimitives.ReadUInt32LittleEndian(footer.Slice(4)) + 1) * 4;
        if (backwardSize != indexSize) throw Corrupt("the stream footer does not point at the index");
        if (BinaryPrimitives.ReadUInt16LittleEndian(footer.Slice(8)) != _streamFlags)
            throw Corrupt("the stream footer flags differ from the header");
        if (footer[10] != (byte)'Y' || footer[11] != (byte)'Z') throw Corrupt("the stream footer magic is missing");
        if (_input.ReadByte() >= 0) throw Corrupt("data follows the end of the xz stream");
    }

    private static ulong ReadVli(byte[] buffer, ref int position)
    {
        ulong value = 0;
        for (var shift = 0; shift < 63; shift += 7)
        {
            if (position >= buffer.Length - 4) throw Corrupt("a block header number is truncated");
            var b = buffer[position++];
            value |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0)
            {
                if (b == 0 && shift != 0) throw Corrupt("a block header number is not minimal");
                return value;
            }
        }
        throw Corrupt("a block header number is oversized");
    }

    internal static InvalidDataException Corrupt(string reason) =>
        new($"The installation payload cannot be unpacked: {reason}.");

    // --- LZMA2 -----------------------------------------------------------------------------------------------

    /// <summary>LZMA2 chunks over one circular dictionary; each call yields one chunk's uncompressed bytes.</summary>
    private sealed class Lzma2Decoder
    {
        private readonly LzmaDecoder _lzma;
        private byte[] _packed = new byte[64 * 1024];
        private bool _needDictionaryReset;
        private bool _needProperties;
        private bool _ended;

        public Lzma2Decoder(int dictionarySize, int maxChunk)
        {
            DictionarySize = dictionarySize;
            _lzma = new LzmaDecoder(dictionarySize, maxChunk);
        }

        public int DictionarySize { get; }

        public static int DictionarySizeFromProperty(byte property)
        {
            if (property > 40) throw Corrupt("the LZMA2 dictionary size is invalid");
            if (property == 40) return int.MaxValue;
            return (2 | (property & 1)) << (property / 2 + 11);
        }

        public void StartBlock()
        {
            _needDictionaryReset = true;
            _needProperties = true;
            _ended = false;
        }

        /// <summary>Decodes the next chunk; returns 0 at the end marker.</summary>
        public int DecodeNextChunk(XzDecoderStream owner, out ReadOnlySpan<byte> output)
        {
            output = default;
            if (_ended) throw Corrupt("LZMA2 data continues after its end marker");
            var control = owner.ReadByteOrThrow();
            if (control == 0x00)
            {
                _ended = true;
                return 0;
            }

            if (control is 0x01 or 0x02)
            {
                // As in liblzma, a dictionary reset means the next LZMA chunk must bring its properties.
                if (control == 0x01) { _lzma.ResetDictionary(); _needDictionaryReset = false; _needProperties = true; }
                else if (_needDictionaryReset) throw Corrupt("the first LZMA2 chunk does not reset the dictionary");
                var size = ((owner.ReadByteOrThrow() << 8) | owner.ReadByteOrThrow()) + 1;
                EnsurePacked(size);
                owner.ReadInput(_packed.AsSpan(0, size));
                output = _lzma.CopyUncompressed(_packed.AsSpan(0, size));
                return size;
            }

            if (control < 0x80) throw Corrupt($"LZMA2 control byte 0x{control:X2} is invalid");
            var unpacked = ((control & 0x1F) << 16) + ((owner.ReadByteOrThrow() << 8) | owner.ReadByteOrThrow()) + 1;
            var packedSize = ((owner.ReadByteOrThrow() << 8) | owner.ReadByteOrThrow()) + 1;
            var reset = (control >> 5) & 0x03;
            if (reset == 3) { _lzma.ResetDictionary(); _needDictionaryReset = false; }
            else if (_needDictionaryReset) throw Corrupt("the first LZMA2 chunk does not reset the dictionary");
            if (reset >= 2)
            {
                _lzma.SetProperties(owner.ReadByteOrThrow());
                _needProperties = false;
            }
            else if (_needProperties) throw Corrupt("an LZMA2 chunk needs properties that were never set");
            if (reset >= 1) _lzma.ResetState();

            EnsurePacked(packedSize);
            owner.ReadInput(_packed.AsSpan(0, packedSize));
            output = _lzma.DecodeChunk(_packed, packedSize, unpacked);
            return unpacked;
        }

        private void EnsurePacked(int size)
        {
            if (_packed.Length < size) _packed = new byte[size];
        }
    }

    // --- LZMA ------------------------------------------------------------------------------------------------

    private sealed class LzmaDecoder
    {
        private const int NumStates = 12;
        private const int PosStatesMax = 1 << 4;
        private const int EndPosModelIndex = 14;
        private const int NumFullDistances = 128;
        private const int AlignBits = 4;
        private const int MatchMinLength = 2;

        // The circular window: one dictionary of history plus room for the chunk being decoded, so a chunk
        // can always be handed out contiguously from ChunkStart without overwriting history it references.
        private readonly byte[] _window;
        private readonly int _dictionarySize;
        private int _windowPosition;
        private long _totalPosition;
        private byte[] _chunk;

        private int _lc, _lp, _pb;
        private ushort[] _literal = [];
        private readonly ushort[] _isMatch = new ushort[NumStates * PosStatesMax];
        private readonly ushort[] _isRep = new ushort[NumStates];
        private readonly ushort[] _isRepG0 = new ushort[NumStates];
        private readonly ushort[] _isRepG1 = new ushort[NumStates];
        private readonly ushort[] _isRepG2 = new ushort[NumStates];
        private readonly ushort[] _isRep0Long = new ushort[NumStates * PosStatesMax];
        private readonly ushort[] _posSlot = new ushort[4 * 64];
        private readonly ushort[] _posSpecial = new ushort[NumFullDistances - EndPosModelIndex + 1];
        private readonly ushort[] _align = new ushort[1 << AlignBits];
        private readonly LengthDecoder _length = new();
        private readonly LengthDecoder _repLength = new();
        private int _state;
        private uint _rep0, _rep1, _rep2, _rep3;

        // Range decoder over the current packed chunk.
        private byte[] _in = [];
        private int _inPosition;
        private int _inEnd;
        private uint _range;
        private uint _code;

        public LzmaDecoder(int dictionarySize, int maxChunk)
        {
            _dictionarySize = dictionarySize;
            _window = new byte[checked(dictionarySize + maxChunk)];
            _chunk = new byte[maxChunk];
        }

        public void ResetDictionary()
        {
            _windowPosition = 0;
            _totalPosition = 0;
        }

        public void SetProperties(int properties)
        {
            if (properties >= 9 * 5 * 5) throw Corrupt("the LZMA properties are invalid");
            _lc = properties % 9;
            properties /= 9;
            _lp = properties % 5;
            _pb = properties / 5;
            if (_lc + _lp > 4) throw Corrupt("the LZMA2 literal properties exceed lc + lp = 4");
            var literalSize = 0x300 << (_lc + _lp);
            if (_literal.Length != literalSize) _literal = new ushort[literalSize];
        }

        public void ResetState()
        {
            Array.Fill(_literal, (ushort)1024);
            Array.Fill(_isMatch, (ushort)1024);
            Array.Fill(_isRep, (ushort)1024);
            Array.Fill(_isRepG0, (ushort)1024);
            Array.Fill(_isRepG1, (ushort)1024);
            Array.Fill(_isRepG2, (ushort)1024);
            Array.Fill(_isRep0Long, (ushort)1024);
            Array.Fill(_posSlot, (ushort)1024);
            Array.Fill(_posSpecial, (ushort)1024);
            Array.Fill(_align, (ushort)1024);
            _length.Reset();
            _repLength.Reset();
            _state = 0;
            _rep0 = _rep1 = _rep2 = _rep3 = 0;
        }

        public ReadOnlySpan<byte> CopyUncompressed(ReadOnlySpan<byte> data)
        {
            foreach (var b in data) PutByte(b);
            return Snapshot(data.Length);
        }

        public ReadOnlySpan<byte> DecodeChunk(byte[] packed, int packedSize, int unpacked)
        {
            _in = packed;
            _inPosition = 0;
            _inEnd = packedSize;
            if (packedSize < 5 || packed[0] != 0) throw Corrupt("an LZMA chunk has an invalid range coder start");
            _code = BinaryPrimitives.ReadUInt32BigEndian(packed.AsSpan(1));
            _range = 0xFFFFFFFF;
            _inPosition = 5;

            var start = _totalPosition;
            var target = start + unpacked;
            var pbMask = (1 << _pb) - 1;
            var lpMask = (1 << _lp) - 1;
            while (_totalPosition < target)
            {
                var posState = (int)_totalPosition & pbMask;
                if (DecodeBit(_isMatch, (_state << 4) + posState) == 0)
                {
                    var previous = _totalPosition == 0 ? 0 : _window[PreviousIndex(0)];
                    var probabilities = 0x300 * ((((int)_totalPosition & lpMask) << _lc) + (previous >> (8 - _lc)));
                    int symbol = 1;
                    if (_state < 7)
                    {
                        while (symbol < 0x100) symbol = (symbol << 1) | DecodeBit(_literal, probabilities + symbol);
                    }
                    else
                    {
                        int matchByte = _window[PreviousIndex(_rep0)];
                        while (symbol < 0x100)
                        {
                            var matchBit = (matchByte >> 7) & 1;
                            matchByte <<= 1;
                            var bit = DecodeBit(_literal, probabilities + 0x100 + (matchBit << 8) + symbol);
                            symbol = (symbol << 1) | bit;
                            if (matchBit != bit)
                            {
                                while (symbol < 0x100) symbol = (symbol << 1) | DecodeBit(_literal, probabilities + symbol);
                                break;
                            }
                        }
                    }
                    PutByte((byte)symbol);
                    _state = _state < 4 ? 0 : _state < 10 ? _state - 3 : _state - 6;
                    continue;
                }

                int length;
                if (DecodeBit(_isRep, _state) == 1)
                {
                    if (_totalPosition == 0) throw Corrupt("an LZMA chunk repeats data before any was written");
                    if (DecodeBit(_isRepG0, _state) == 0)
                    {
                        if (DecodeBit(_isRep0Long, (_state << 4) + posState) == 0)
                        {
                            if (_rep0 >= _totalPosition)
                                throw Corrupt("an LZMA repeat reaches before the start of the dictionary");
                            _state = _state < 7 ? 9 : 11;
                            PutByte(_window[PreviousIndex(_rep0)]);
                            continue;
                        }
                    }
                    else
                    {
                        uint distance;
                        if (DecodeBit(_isRepG1, _state) == 0) distance = _rep1;
                        else
                        {
                            if (DecodeBit(_isRepG2, _state) == 0) distance = _rep2;
                            else { distance = _rep3; _rep3 = _rep2; }
                            _rep2 = _rep1;
                        }
                        _rep1 = _rep0;
                        _rep0 = distance;
                    }
                    length = _repLength.Decode(this, posState);
                    _state = _state < 7 ? 8 : 11;
                }
                else
                {
                    _rep3 = _rep2;
                    _rep2 = _rep1;
                    _rep1 = _rep0;
                    length = _length.Decode(this, posState);
                    _state = _state < 7 ? 7 : 10;
                    _rep0 = DecodeDistance(length);
                    if (_rep0 == 0xFFFFFFFF) throw Corrupt("an LZMA2 chunk contains an end marker");
                }

                length += MatchMinLength;
                if (_rep0 >= _totalPosition || _rep0 >= (uint)_dictionarySize)
                    throw Corrupt("an LZMA match reaches before the start of the dictionary");
                if (_totalPosition + length > target) throw Corrupt("an LZMA match runs past the end of its chunk");
                CopyMatch((int)_rep0, length);
            }

            if (_inPosition != _inEnd || _code != 0) throw Corrupt("an LZMA chunk's size does not match its data");
            return Snapshot(unpacked);
        }

        private uint DecodeDistance(int length)
        {
            var lengthState = length < 4 ? length : 3;
            var slot = BitTree(_posSlot, lengthState << 6, 6);
            if (slot < 4) return (uint)slot;
            var directBits = (slot >> 1) - 1;
            var distance = (uint)((2 | (slot & 1)) << directBits);
            if (slot < EndPosModelIndex)
                return distance + ReverseBitTree(_posSpecial, (int)distance - slot - 1, directBits);
            distance += DirectBits(directBits - AlignBits) << AlignBits;
            return distance + ReverseBitTree(_align, 0, AlignBits);
        }

        private int PreviousIndex(uint distance)
        {
            var index = _windowPosition - 1 - (long)distance;
            if (index < 0) index += _window.Length;
            return (int)index;
        }

        private void PutByte(byte value)
        {
            _window[_windowPosition] = value;
            if (++_windowPosition == _window.Length) _windowPosition = 0;
            _totalPosition++;
        }

        private void CopyMatch(int distance, int length)
        {
            var source = PreviousIndex((uint)distance);
            for (var i = 0; i < length; i++)
            {
                _window[_windowPosition] = _window[source];
                if (++_windowPosition == _window.Length) _windowPosition = 0;
                if (++source == _window.Length) source = 0;
            }
            _totalPosition += length;
        }

        /// <summary>The last <paramref name="count"/> bytes written, as one contiguous span.</summary>
        private ReadOnlySpan<byte> Snapshot(int count)
        {
            var start = _windowPosition - count;
            if (start >= 0) return _window.AsSpan(start, count);
            start += _window.Length;
            if (_chunk.Length < count) _chunk = new byte[count];
            var head = _window.Length - start;
            _window.AsSpan(start, head).CopyTo(_chunk);
            _window.AsSpan(0, count - head).CopyTo(_chunk.AsSpan(head));
            return _chunk.AsSpan(0, count);
        }

        internal int DecodeBit(ushort[] probabilities, int index)
        {
            uint probability = probabilities[index];
            var bound = (_range >> 11) * probability;
            int bit;
            if (_code < bound)
            {
                _range = bound;
                probabilities[index] = (ushort)(probability + ((2048 - probability) >> 5));
                bit = 0;
            }
            else
            {
                _range -= bound;
                _code -= bound;
                probabilities[index] = (ushort)(probability - (probability >> 5));
                bit = 1;
            }
            if (_range < (1u << 24))
            {
                _range <<= 8;
                _code = (_code << 8) | NextInput();
            }
            return bit;
        }

        private uint DirectBits(int count)
        {
            uint result = 0;
            for (; count > 0; count--)
            {
                _range >>= 1;
                var t = (_code - _range) >> 31;
                _code -= _range & (t - 1);
                result = (result << 1) | (1 - t);
                if (_range < (1u << 24))
                {
                    _range <<= 8;
                    _code = (_code << 8) | NextInput();
                }
            }
            return result;
        }

        internal int BitTree(ushort[] probabilities, int offset, int bits)
        {
            var m = 1;
            for (var i = 0; i < bits; i++) m = (m << 1) | DecodeBit(probabilities, offset + m);
            return m - (1 << bits);
        }

        private uint ReverseBitTree(ushort[] probabilities, int offset, int bits)
        {
            var m = 1;
            uint symbol = 0;
            for (var i = 0; i < bits; i++)
            {
                var bit = DecodeBit(probabilities, offset + m);
                m = (m << 1) | bit;
                symbol |= (uint)bit << i;
            }
            return symbol;
        }

        private uint NextInput()
        {
            if (_inPosition >= _inEnd) throw Corrupt("an LZMA chunk reads past its compressed size");
            return _in[_inPosition++];
        }

        private sealed class LengthDecoder
        {
            private readonly ushort[] _choice = new ushort[2];
            private readonly ushort[] _low = new ushort[PosStatesMax << 3];
            private readonly ushort[] _mid = new ushort[PosStatesMax << 3];
            private readonly ushort[] _high = new ushort[256];

            public void Reset()
            {
                Array.Fill(_choice, (ushort)1024);
                Array.Fill(_low, (ushort)1024);
                Array.Fill(_mid, (ushort)1024);
                Array.Fill(_high, (ushort)1024);
            }

            public int Decode(LzmaDecoder decoder, int posState)
            {
                if (decoder.DecodeBit(_choice, 0) == 0) return decoder.BitTree(_low, posState << 3, 3);
                if (decoder.DecodeBit(_choice, 1) == 0) return 8 + decoder.BitTree(_mid, posState << 3, 3);
                return 16 + decoder.BitTree(_high, 0, 8);
            }
        }
    }

    // --- x86 BCJ ---------------------------------------------------------------------------------------------

    /// <summary>
    /// Reverses xz's x86 branch converter: E8/E9 call and jump targets were made absolute before compression.
    /// Up to four trailing bytes are held back until the next chunk shows whether they start an instruction.
    /// </summary>
    private sealed class BcjX86Filter(uint startOffset)
    {
        private static readonly bool[] MaskToAllowed = [true, true, true, false, true, false, false, false];
        private static readonly int[] MaskToBitNumber = [0, 1, 2, 2, 3, 3, 3, 3];
        private byte[] _buffer = new byte[MaxChunkUnpacked + 8];
        private int _held;
        private uint _position = startOffset;
        private uint _previousMask;

        public ReadOnlySpan<byte> Filter(ReadOnlySpan<byte> input)
        {
            if (_buffer.Length < _held + input.Length) Array.Resize(ref _buffer, _held + input.Length);
            input.CopyTo(_buffer.AsSpan(_held));
            var size = _held + input.Length;
            var done = Convert(_buffer.AsSpan(0, size));
            _position += (uint)done;
            var output = _buffer.AsSpan(0, done).ToArray();
            _held = size - done;
            _buffer.AsSpan(done, _held).CopyTo(_buffer);
            return output;
        }

        public ReadOnlySpan<byte> Flush()
        {
            var output = _buffer.AsSpan(0, _held).ToArray();
            _position += (uint)_held;
            _held = 0;
            return output;
        }

        private static bool TestMsByte(byte b) => b == 0x00 || b == 0xFF;

        private int Convert(Span<byte> buffer)
        {
            if (buffer.Length <= 4) return 0;
            var limit = buffer.Length - 4;
            long previousPosition = -1;
            var previousMask = _previousMask;
            int i;
            for (i = 0; i < limit; i++)
            {
                if ((buffer[i] & 0xFE) != 0xE8) continue;

                var distance = i - previousPosition;
                if (distance > 3) previousMask = 0;
                else
                {
                    previousMask = (previousMask << (int)(distance - 1)) & 7;
                    if (previousMask != 0)
                    {
                        var b = buffer[i + 4 - MaskToBitNumber[previousMask]];
                        if (!MaskToAllowed[previousMask] || TestMsByte(b))
                        {
                            previousPosition = i;
                            previousMask = (previousMask << 1) | 1;
                            continue;
                        }
                    }
                }
                previousPosition = i;

                if (TestMsByte(buffer[i + 4]))
                {
                    var source = BinaryPrimitives.ReadUInt32LittleEndian(buffer.Slice(i + 1));
                    uint destination;
                    while (true)
                    {
                        destination = source - (_position + (uint)i + 5);
                        if (previousMask == 0) break;
                        var j = MaskToBitNumber[previousMask] * 8;
                        var b = (byte)(destination >> (24 - j));
                        if (!TestMsByte(b)) break;
                        source = destination ^ ((1u << (32 - j)) - 1);
                    }
                    destination &= 0x01FFFFFF;
                    destination |= 0u - (destination & 0x01000000);
                    BinaryPrimitives.WriteUInt32LittleEndian(buffer.Slice(i + 1), destination);
                    i += 4;
                }
                else
                {
                    previousMask = (previousMask << 1) | 1;
                }
            }

            var tail = i - previousPosition;
            _previousMask = tail > 3 ? 0 : previousMask << (int)(tail - 1);
            return i;
        }
    }

    // --- checksums -------------------------------------------------------------------------------------------

    internal struct Crc32
    {
        private static readonly uint[] Table = BuildTable();
        private uint _state;

        public readonly uint Value => _state;

        public void Append(ReadOnlySpan<byte> data)
        {
            var crc = ~_state;
            foreach (var b in data) crc = Table[(byte)(crc ^ b)] ^ (crc >> 8);
            _state = ~crc;
        }

        public static uint Compute(ReadOnlySpan<byte> data)
        {
            var crc = new Crc32();
            crc.Append(data);
            return crc.Value;
        }

        private static uint[] BuildTable()
        {
            var table = new uint[256];
            for (uint i = 0; i < 256; i++)
            {
                var value = i;
                for (var bit = 0; bit < 8; bit++) value = (value & 1) != 0 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
                table[i] = value;
            }
            return table;
        }
    }

    internal struct Crc64
    {
        private static readonly ulong[] Table = BuildTable();
        private ulong _state;

        public readonly ulong Value => _state;

        public void Append(ReadOnlySpan<byte> data)
        {
            var crc = ~_state;
            foreach (var b in data) crc = Table[(byte)(crc ^ b)] ^ (crc >> 8);
            _state = ~crc;
        }

        private static ulong[] BuildTable()
        {
            var table = new ulong[256];
            for (ulong i = 0; i < 256; i++)
            {
                var value = i;
                for (var bit = 0; bit < 8; bit++)
                    value = (value & 1) != 0 ? 0xC96C5795D7870F42UL ^ (value >> 1) : value >> 1;
                table[i] = value;
            }
            return table;
        }
    }
}
