import os
import mmap
import struct
import sys

# version 2 introduced thread_name
_format_version = 2

def nanos_to_millis(nanos):
    return float(nanos) / 1000000

def nanos_to_seconds(nanos):
    return float(nanos) / 1000000000

def format_duration(time):
    return "%4.3f" % nanos_to_millis(time)

def format_time(time):
    return "%12.9f" % nanos_to_seconds(time)

class BacktraceFormatter:
    def __init__(self, resolver):
        self.resolver = resolver

    def __call__(self, backtrace):
        if not backtrace:
            return ''

        while self.resolver(backtrace[0] - 1).startswith("tracepoint"):
            backtrace.pop(0)

        return '   [' + ', '.join((str(self.resolver(x - 1)) for x in backtrace if x)) + ']'

def simple_symbol_formatter(addr):
    return '0x%x' % addr

default_backtrace_formatter = BacktraceFormatter(simple_symbol_formatter)

class TimeRange(object):
    """
    Represents time from @begin inclusive to @end exclusive.
    None in any of these means open range from that side.

    """
    def __init__(self, begin, end):
        self.begin = begin
        self.end = end

    def __contains__(self, timestamp):
        if self.begin and timestamp < self.begin:
            return False
        if self.end and timestamp >= self.end:
            return False
        return True

    def intersection(self, other):
        begin = max(self.begin, other.begin)

        if self.end is None:
            end = other.end
        elif other.end is None:
            end = self.end
        else:
            end = min(self.end, other.end)

        if begin and end and begin > end:
            return None

        return TimeRange(begin, end)

class TracePoint:
    def __init__(self, key, name, signature, format):
        self.key = key
        self.name = name
        self.signature = signature
        self.format = format

    def __repr__(self):
        return 'TracePoint(key=%s, name=%s, signature=%s, format=%s)' % (
                self.key,
                self.name,
                self.signature,
                self.format)

class Trace:
    def __init__(self, tp, thread, thread_name, time, cpu, data, backtrace=None):
        self.tp = tp
        self.thread = thread
        self.thread_name = thread_name
        self.time = time
        self.cpu = cpu
        self.data = data
        self.backtrace = backtrace

    @property
    def name(self):
        return self.tp.name

    def format_data(self):
        format = self.tp.format
        format = format.replace('%p', '0x%016x')
        return format % self.data

    def format(self, bt_formatter=default_backtrace_formatter):
        return '0x%016x %-15s %2d %19s %-20s %s%s' % (
            self.thread,
            self.thread_name,
            self.cpu,
            format_time(self.time),
            self.name,
            self.format_data(),
            bt_formatter(self.backtrace))

    def __str__(self):
        return self.format()

class TimedTrace:
    def __init__(self, trace):
        self.trace = trace
        self.duration = None

    @property
    def duration(self):
        return self.duration

    @property
    def time(self):
        return self.trace.time

    @property
    def time_range(self):
        return TimeRange(self.trace.time, self.trace.time + self.duration)

class SlidingUnpacker:
    def __init__(self, buffer):
        self.buffer = buffer
        self.offset = 0

    def unpack_str(self):
        len, = struct.unpack_from('H', self.buffer, offset=self.offset)
        self.offset += struct.calcsize('H')
        string = ''.join(struct.unpack_from('c' * len, self.buffer, offset=self.offset))
        self.offset += len
        return string

    def unpack(self, format):
        values = struct.unpack_from(format, self.buffer, offset=self.offset)
        self.offset += struct.calcsize(format)
        return values

    def __nonzero__(self):
        return self.offset < len(self.buffer)

class WritingPacker:
    def __init__(self, writer):
        self.writer = writer

    def pack(self, format, *data):
        self.writer(struct.pack(format, *data))

    def pack_str(self, *args):
        for arg in args:
            if not type(arg) == str:
                raise Exception('Should be string but is %s' % type(arg))

            count = len(arg)
            self.writer(struct.pack('H', count))
            self.writer(struct.pack('c' * count, *(map(str.encode, arg))))

def read(buffer_view):
    unpacker = SlidingUnpacker(buffer_view)
    version, = unpacker.unpack('i')

    if version != _format_version:
        raise Exception('Version mismatch, current is %d got %d' % (_format_version, version))

    tracepoints = {}
    n_tracepoints, = unpacker.unpack('Q')
    for i in range(n_tracepoints):
        key, = unpacker.unpack('Q')
        tracepoints[key] = TracePoint(key, unpacker.unpack_str(),
            unpacker.unpack_str(), unpacker.unpack_str())

    while unpacker:
        tp_key, thread, thread_name, time, cpu = unpacker.unpack('QQ16sQI')
        thread_name = thread_name.rstrip('\0')
        tp = tracepoints[tp_key]

        backtrace = []
        while True:
            frame, = unpacker.unpack('Q')
            if not frame:
                break
            backtrace.append(frame)

        data = unpacker.unpack(tp.signature)
        yield Trace(tp, thread, thread_name, time, cpu, data, backtrace=backtrace)

def write(traces, writer):
    packer = WritingPacker(writer)
    packer.pack('i', _format_version)

    tracepoints = set(trace.tp for trace in traces)
    packer.pack('Q', len(tracepoints))
    for tp in tracepoints:
        packer.pack('Q', tp.key)
        packer.pack_str(tp.name, tp.signature, tp.format)

    for trace in traces:
        packer.pack('QQ16sQI', trace.tp.key, trace.thread, trace.thread_name.encode(),
                    trace.time, trace.cpu)

        if trace.backtrace:
            for frame in filter(None, trace.backtrace):
                packer.pack('Q', frame)
        packer.pack('Q', 0)

        packer.pack(trace.tp.signature, *trace.data)

class read_file:
    def __init__(self, filename):
        self.filename = filename
        self.map = None

    def __enter__(self):
        self.file = open(self.filename, 'r+b')
        self.map = mmap.mmap(self.file.fileno(), 0)
        return self

    def __exit__(self, *args):
        if self.map:
            self.map.close()
        self.file.close()

    def get_traces(self):
        return read(self.map)

def write_to_file(filename, traces):
    with open(filename, 'wb') as file:
        return write(traces, file.write)
