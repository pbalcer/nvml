#!../env.py
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2019-2020, Intel Corporation
#

from os import path
import testframework as t


class BASE(t.BaseTest):
    test_type = t.Medium

    def run(self, ctx):
        ctx.exec('obj_many_pools', ctx.testdir)


class TEST0(BASE):
    "many pools test"
