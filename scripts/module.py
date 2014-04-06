#!/usr/bin/python

import sys
import re
import os
import subprocess
import operator
import argparse
import textwrap
from functools import reduce
from osv.modules import api, resolve, filemap

class jvm(api.basic_app):
    multimain_manifest = '/etc/javamains'
    apps = []

    def prepare_manifest(self, build_dir, manifest_type, manifest):
        if manifest_type != 'usr':
            return

        javamains_path = os.path.join(build_dir, 'javamains')
        with open(javamains_path, "w") as mains:
            for app in self.apps:
                mains.write('\n'.join(app.get_multimain_lines()) + '\n')

        manifest.write('%s:%s\n' % (self.multimain_manifest, javamains_path))

    def get_launcher_args(self):
        jvm_args = []
        for app in self.apps:
            jvm_args.extend(app.get_jvm_args())

        return ['java.so'] + jvm_args + ['io.osv.MultiJarLoader', '-mains', self.multimain_manifest]

    def add(self, app):
        self.apps.append(app)

def expand(text, variables):
    def resolve(m):
        name = m.group('name')
        if not name in variables:
            raise Exception('Undefined variable: ' + name)
        return variables[name]

    return re.sub(r'\${(?P<name>.*)}', resolve, text)

def append_manifest(file_path, dst_file, variables={}):
    with open(file_path) as src_file:
        for line in src_file:
            line = line.rstrip()
            if line != '[manifest]':
                dst_file.write(expand(line + '\n', variables))

def generate_manifests(modules, basic_apps):
    for manifest_type in ["usr", "bootfs"]:
        manifest_name = "%s.manifest" % manifest_type
        print("Preparing %s" % manifest_name)

        with open(os.path.join(resolve.get_build_path(), manifest_name), "w") as manifest:
            manifest.write('[manifest]\n')

            append_manifest(os.path.join(resolve.get_osv_base(), "%s.skel" % manifest_name), manifest)

            for module in modules:
                module_manifest = os.path.join(module.local_path, manifest_name)

                if os.path.exists(module_manifest):
                    print("Appending %s to %s" % (module_manifest, manifest_name))
                    append_manifest(module_manifest, manifest, variables={
                        'MODULE_DIR': module.local_path,
                        'OSV_BASE': resolve.get_osv_base()
                    })

                filemap_attr = '%s_files' % manifest_type
                if hasattr(module, filemap_attr):
                    filemap.as_manifest(getattr(module, filemap_attr), manifest.write)

            for app in basic_apps:
                app.prepare_manifest(resolve.get_build_path(), manifest_type, manifest)

def format_args(args):
    if isinstance(args, str):
        return args
    else:
        return ' '.join(args)

def get_command_line(basic_apps):
    return '&'.join((format_args(app.get_launcher_args()) for app in basic_apps))

def make_modules(modules):
    for module in modules:
        if os.path.exists(os.path.join(module.local_path, 'Makefile')):
            if subprocess.call(["make module"], shell=True, cwd=module.local_path):
                raise Exception('make failed for ' + module.name)

def flatten_list(elememnts):
    if not elememnts:
        return []
    if not isinstance(elememnts, list):
        return [elememnts]
    return reduce(operator.add, [flatten_list(e) for e in elememnts])

def get_basic_apps(apps):
    basic_apps = []
    _jvm = jvm()

    for app in flatten_list(apps):
        if isinstance(app, api.basic_app):
            basic_apps.append(app)
        elif isinstance(app, api.java_app):
            _jvm.add(app)
        else:
            raise Exception("Unknown app type: " + str(app))

    if _jvm.apps:
        basic_apps.append(_jvm)

    return basic_apps

def generate_cmdline(apps):
    cmdline_path = os.path.join(resolve.get_build_path(), "cmdline")
    print("Saving command line to %s" % cmdline_path)
    with open(cmdline_path, "w") as cmdline_file:
        if apps:
            cmdline_file.write(get_command_line(apps))
        else:
            print("No apps selected")

if __name__ == "__main__":
    image_configs_dir = resolve.get_images_dir()

    parser = argparse.ArgumentParser(prog='module.py')
    parser.add_argument("-c", "--image-config", action="store", default="default",
                        help="image configuration name. Looked up in " + image_configs_dir)
    args = parser.parse_args()

    image_config_file = os.path.join(image_configs_dir, args.image_config + '.py')
    if os.path.exists(image_config_file):
        print("Using image config: %s" % image_config_file)
        config = resolve.local_import(image_config_file)
        run_list = config.get('run', [])
    else:
        # If images/image_config doesn't exist, assume image_config is a
        # comma-separated list of module names, and build an image from those
        # modules (and their dependencies). The command line to run is to
        # run each of the module's "default" command line, in parallel.
        # You can choose a module's non-default command line, with a dot,
        # e.g.: mgmt.shell use's mgmt's "shell" command line.
        print("No such image configuration: " + args.image_config + ". Assuming list of modules.")
        run_list = []
        for module in args.image_config.split(","):
            a = module.split(".", 1)
            name = a[0]
            variant = a[1] if (len(a) > 1) else "default"
            mod = api.require(name)
            if hasattr(mod, variant):
                run_list.append(getattr(mod, variant));
            elif variant != "default" and variant != "none":
                raise Exception("Attribute %s not set in module %s" % (variant, name))

    modules = resolve.get_required_modules()

    print("Modules:")
    if not modules:
        print("  None")
    for module in modules:
        print("  " + module.name)

    make_modules(modules)

    apps_to_run = get_basic_apps(run_list)
    generate_manifests(modules, apps_to_run)
    generate_cmdline(apps_to_run)
