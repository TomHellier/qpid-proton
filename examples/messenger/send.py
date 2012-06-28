#!/usr/bin/python
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
import sys, optparse
from xproton import *

parser = optparse.OptionParser(usage="usage: %prog [options] <msg_1> ... <msg_n>",
                               description="simple message sender")
parser.add_option("-a", "--address", default="//0.0.0.0",
                  help="address: //<domain>[/<name>] (default %default)")

opts, args = parser.parse_args()
if not args:
  args = ["Hello World!"]

mng = pn_messenger(None)
pn_messenger_start(mng)

msg = pn_message()
for m in args:
  pn_message_set_address(msg, opts.address)
  pn_message_load(msg, m)
  if pn_messenger_put(mng, msg):
    print pn_messenger_error(mng)
    break

if pn_messenger_send(mng):
  print pn_messenger_error(mng)
else:
  print "sent:", ", ".join(args)

pn_messenger_stop(mng)
pn_messenger_free(mng)
