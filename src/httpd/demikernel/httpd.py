#!/usr/bin/env python3
from research.stackbench.demikernel.scripts import *
import argparse
import pathlib

class DemiRustHttpdServer(NopHttpdInterface):
    def __init__(self, node, env, client_netif):
        super().__init__(1)
        self.node = node
        self.env = env
        self.client_netif = client_netif
    def get_procnames(self):
        return ["httpd.elf"]
    def build(self):
        curdir = pathlib.Path(__file__).parent.absolute()
        self.node.rsync(f'{curdir}/httpd.rs', f'{self.node.dir}/examples/rust/')
        self.node.check('make LIBOS=catnip all-examples-rust')
    def setup(self):
        self.env.setup(self.node, self.cpu_num, [self.client_netif])
    def run(self, wrapper, option, runner):
        if wrapper:
            raise Exception('not supported')
        ip4 = self.node.netif_info.container['ip4']
        runner.execute(f'ARGS="--address {ip4}:80 {option.get()}" TEST=httpd sudo -E make LIBOS=catnip run-rust')
    def get_node(self):
        return self.node
    @staticmethod
    def create1(conf):
        node = conf.get_server()
        client_netif = conf.client.netif
        env = DemiEnv()
        return DemiRustHttpdServer(node, env, client_netif)

