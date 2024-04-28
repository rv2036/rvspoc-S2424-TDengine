###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-


from util.log import *
from util.cases import *
from util.sql import *
from util.common import *
from util.sqlset import *
from util.autogen import *

import random
import time
import traceback
import os
from   os import path


class TDTestCase:
    # init 
    def init(self, conn, logSql, replicaVar=1):
        self.replicaVar = int(replicaVar)
        tdLog.debug("start to execute %s" % __file__)
        tdSql.init(conn.cursor(), True)
        
        # autoGen
        self.autoGen = AutoGen()
                        
    def waitTranslation(self, waitSeconds):
        # wait end
        for i in range(waitSeconds):
            sql ="show transactions;"
            rows = tdSql.query(sql)
            if rows == 0:
               return True
            tdLog.info(f"i={i} wait for translation finish ...")
            time.sleep(1)

        return False

    def getPath(self, tool="taosBenchmark"):
        if (platform.system().lower() == 'windows'):
            tool = tool + ".exe"
        selfPath = os.path.dirname(os.path.realpath(__file__))

        if ("community" in selfPath):
            projPath = selfPath[:selfPath.find("community")]
        else:
            projPath = selfPath[:selfPath.find("tests")]

        paths = []
        for root, dirs, files in os.walk(projPath):
            if ((tool) in files):
                rootRealPath = os.path.dirname(os.path.realpath(root))
                if ("packaging" not in rootRealPath):
                    paths.append(os.path.join(root, tool))
                    break
        if (len(paths) == 0):
            tdLog.exit("taosBenchmark not found!")
            return
        else:
            tdLog.info("taosBenchmark found in %s" % paths[0])
            return paths[0]

    def taosBenchmark(self, param):
        binPath = self.getPath()
        cmd = f"{binPath} {param}"
        tdLog.info(cmd)
        os.system(cmd)

    def case1(self):

        tdSql.execute(f'create database if not exists d1 vgroups 1')
        tdSql.execute(f'use d1')
        tdSql.execute(f'create table st(ts timestamp, i int) tags(t int)')
        tdSql.execute(f'insert into t1 using st tags(1) values(now, 1) (now+1s, 2)')
        tdSql.execute(f'insert into t2 using st tags(2) values(now, 1) (now+1s, 2)')
        tdSql.execute(f'insert into t3 using st tags(3) values(now, 1) (now+1s, 2)')

        tdSql.execute("create stream stream1 fill_history 1 into sta subtable(concat('new-', tname)) AS SELECT "
                      "_wstart, count(*), avg(i) FROM st PARTITION BY tbname tname INTERVAL(1m)", show=True)

        tdSql.execute("create stream stream2 fill_history 1 into stb subtable(concat('new-', tname)) AS SELECT "
                      "_wstart, count(*), avg(i) FROM st PARTITION BY tbname tname INTERVAL(1m)", show=True)

        time.sleep(2)
        tdSql.query("select * from sta")
        tdSql.checkRows(3)
        tdSql.query("select tbname from sta order by tbname")
        if not tdSql.getData(0, 0).startswith('new-t1_1.d1.sta_'):
            tdLog.exit("error1")

        if not tdSql.getData(1, 0).startswith('new-t2_1.d1.sta_'):
            tdLog.exit("error2")

        if not tdSql.getData(2, 0).startswith('new-t3_1.d1.sta_'):
            tdLog.exit("error3")

        tdSql.query("select * from stb")
        tdSql.checkRows(3)
        tdSql.query("select tbname from stb order by tbname")
        if not tdSql.getData(0, 0).startswith('new-t1_1.d1.stb_'):
            tdLog.exit("error4")

        if not tdSql.getData(1, 0).startswith('new-t2_1.d1.stb_'):
            tdLog.exit("error5")

        if not tdSql.getData(2, 0).startswith('new-t3_1.d1.stb_'):
            tdLog.exit("error6")

    # run
    def run(self):
        self.case1()
        # gen data
        random.seed(int(time.time()))
        self.taosBenchmark(" -d db -t 2 -v 2 -n 1000000 -y")
        # create stream
        tdSql.execute("use db")
        tdSql.execute("create stream stream3 fill_history 1 into sta as select count(*) as cnt from meters interval(10a);",show=True)
        sql = "select count(*) from sta"
        # loop wait max 60s to check count is ok
        tdLog.info("loop wait result ...")
        time.sleep(5)

        tdSql.checkDataLoop(0, 0, 100000, sql, loopCount=120, waitTime=0.5)

        time.sleep(5)

        # check all data is correct
        sql = "select * from sta where cnt != 20;"
        tdSql.query(sql)
        tdSql.checkRows(0)

        # check ts interval is correct
        sql = "select * from ( select diff(_wstart) as tsdif from sta ) where tsdif != 10;"
        tdSql.query(sql)
        tdSql.checkRows(0)

    # stop
    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)


tdCases.addLinux(__file__, TDTestCase())
