#!/usr/bin/python

import os, sys, struct, optparse, io, subprocess, shutil, socket, time, threading, stat
try:
    import configparser
except ImportError:
    import ConfigParser as configparser

defines = {}

def add_var(option, opt, value, parser):
    var, val = value.split('=')
    defines[var] = val

def expand(items):
    for name, hostname in items:
        if name.endswith('/**') and hostname.endswith('/**'):
            name = name[:-2]
            hostname = hostname[:-2]
            for dirpath, dirnames, filenames in os.walk(hostname):
                for filename in filenames:
                    relpath = dirpath[len(hostname):]
                    if relpath != "" :
                        relpath += "/"
                    yield (name + relpath + filename,
                           hostname + relpath + filename)
        elif '/&/' in name and hostname.endswith('/&'):
            prefix, suffix = name.split('/&/', 1)
            yield (prefix + '/' + suffix, hostname[:-1] + suffix)
        else:
            yield (name, hostname)

def unsymlink(f):
    try:
        link = os.readlink(f)
        if link.startswith('/'):
            # try to find a match
            base = os.path.dirname(f)
            while not os.path.exists(base + link):
                base = os.path.dirname(base)
        else:
            base = os.path.dirname(f) + '/'
        return unsymlink(base + link)
    except Exception:
        return f

def upload(osv, manifest, depends):
    files = dict([(f, manifest.get('manifest', f, vars = defines))
                  for f in manifest.options('manifest')])

    files = list(expand(files.items()))
    files = [(x, unsymlink(y)) for (x, y) in files]

    # Wait for the guest to come up and tell us it's listening
    while True:
        line = osv.stdout.readline().decode()
        if not line or line.find("Waiting for connection")>=0:
            break;
        print(line.rstrip())

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", 10000));

    # We'll want to read the rest of the guest's output, so that it doesn't
    # hang, and so the user can see what's happening. Easiest to do this with
    # a thread.
    def consumeoutput(file):
        for line in iter(lambda: file.readline().decode(), ''):
            print(line.rstrip())
    threading.Thread(target = consumeoutput, args = (osv.stdout,)).start()

    # Send a CPIO header or file, padded to multiple of 4 bytes
    def cpio_send(data):
        s.sendall(data)
        partial = len(data)%4
        if partial > 0:
            s.sendall(b'\0'*(4-partial))
    def cpio_field(number, length):
        return ("%.*x" % (length, number)).encode()
    def cpio_header(filename, mode, filesize):
        return (b"070701"                         # magic
                + cpio_field(0, 8)                # inode
                + cpio_field(mode, 8)             # mode
                + cpio_field(0, 8)                # uid
                + cpio_field(0, 8)                # gid
                + cpio_field(0, 8)                # nlink
                + cpio_field(0, 8)                # mtime
                + cpio_field(filesize, 8)         # filesize
                + cpio_field(0, 8)                # devmajor
                + cpio_field(0, 8)                # devminor
                + cpio_field(0, 8)                # rdevmajor
                + cpio_field(0, 8)                # rdevminor
                + cpio_field(len(filename)+1, 8)  # namesize
                + cpio_field(0, 8)                # check
                + filename.encode() + b'\0')

    def strip_file(filename):
        stripped_filename = filename
        if(filename.endswith(".so") and \
                (filename[0] != "/" or filename.startswith(os.getcwd()))):
            stripped_filename = filename[:-3] + "-stripped.so"
            if(not os.path.exists(stripped_filename) \
                    or (os.path.getmtime(stripped_filename) < \
                        os.path.getmtime(filename))):
                subprocess.call(["strip", "-o", stripped_filename, filename])
        return stripped_filename


    # Send the files to the guest
    for name, hostname in files:
        depends.write(u'\t%s \\\n' % (hostname,))
        hostname = strip_file(hostname)
        if os.path.isdir(hostname) :
            cpio_send(cpio_header(name, stat.S_IFDIR, 0))
        else:
            cpio_send(cpio_header(name, stat.S_IFREG, os.stat(hostname).st_size))
            with open(hostname, 'rb') as f:
                cpio_send(f.read())
    cpio_send(cpio_header("TRAILER!!!", 0, 0))
    s.shutdown(socket.SHUT_WR)

    # Wait for the guest to actually finish writing and syncing
    s.recv(1)
    s.close()

def main():
    make_option = optparse.make_option

    opt = optparse.OptionParser(option_list = [
            make_option('-o',
                        dest = 'output',
                        help = 'write to FILE',
                        metavar = 'FILE'),
            make_option('-d',
                        dest = 'depends',
                        help = 'write dependencies to FILE',
                        metavar = 'FILE',
                        default = None),
            make_option('-m',
                        dest = 'manifest',
                        help = 'read manifest from FILE',
                        metavar = 'FILE'),
            make_option('-D',
                        type = 'string',
                        help = 'define VAR=DATA',
                        metavar = 'VAR=DATA',
                        action = 'callback',
                        callback = add_var),
    ])

    (options, args) = opt.parse_args()

    depends = io.StringIO()
    if options.depends:
        depends = file(options.depends, 'w')
    manifest = configparser.SafeConfigParser()
    manifest.optionxform = str # avoid lowercasing
    manifest.read(options.manifest)

    depends.write(u'%s: \\\n' % (options.output,))

    image_path = os.path.abspath(options.output)
    osv = subprocess.Popen('cd ../..; scripts/run.py -m 512 -c1 -i %s -u -s -e "/tools/cpiod.so" --forward tcp:10000::10000' % image_path, shell = True, stdout=subprocess.PIPE)

    upload(osv, manifest, depends)

    osv.wait()

    depends.write(u'\n\n')
    depends.close()

if __name__ == "__main__":
    main()
