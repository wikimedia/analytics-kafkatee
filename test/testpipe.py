#!/usr/bin/python
#

"""
The testpipe is configured in kafkatee configuration file as an input pipe.
testpipe creates a fifo file which it reads from, and configures kafkatee
to write the output to this fifo.
This way testpipe can generate input for kafkatee as well as check its output,
all in the same process.

'input' referred to in this file corresponds to the kafkatee input,
while 'output' refers to the kafkatee output. testpipe writes to the 'input'
and reads from the 'output'.

The input generator runs in its own thread while the output checker
runs in the main thread.

The FIFO must have been previously created (by command.init=mkfifo <name> in
the kafkatee configuration file).

"""

import argparse
import os
import errno
import time
import threading
import sys
import json
import signal
import uuid


conf = {}

# Sorted list of json obj fields.
json_obj_fields = ('seq','field1','field2','field3','field4')
json_obj = {'seq': 0,
            'field1': 'testid',
            'field2': 'FIELD-TWO',
            'field3': 'FIELD-THREE',
            'field4': 'also seq'}

test_id = ''

failures = 0
outputs_started = 0
outputs_good = 0

def log (str):
    sys.stderr.write('\033[33mTEST:%s: %s\033[0m\n' % \
                         (threading.current_thread().name, str))

def fatal (str):
    global failures
    failures += 1
    raise Exception('\033[31mTEST:%s: FAILED: %s\033[0m' % \
                        (threading.current_thread().name, str))


def input_generator_main ():
    global test_id

    log('started input generator, will generate seqs %i .. %i' %
        (args.seq, args.seq_hi))

    cnt = 0
    for seq in range(args.seq, args.seq_hi):
        obj = json_obj
        obj['seq'] = seq
        obj['field1'] = test_id
        obj['field4'] = seq

        if args.genenc == 'json':
            print json.dumps(obj)
        else:
            print '%d,%s,%s,%s,%s' % (obj['seq'],
                                      obj['field1'],
                                      obj['field2'],
                                      obj['field3'],
                                      obj['field4'])

        cnt += 1
        if (cnt % args.chunk) == 0:
            sys.stdout.flush()
            time.sleep(1)

    sys.stdout.flush()

    log('input generator done, %i messages written' % (cnt))
    time.sleep(args.idle)


def input_generator_start ():
    thr = threading.Thread(None, input_generator_main, 'inputgen')
    thr.daemon = True
    thr.start()


def check_line (o, line):
    global last_seq
    global json_obj
    global test_id

    if args.checkenc == 'json':
        obj = json.loads(line)
    else:
        arr = line.split(',')
        obj = {}
        i = 0
        for f in json_obj_fields:
            obj[f] = arr[i]
            i += 1

    if not 'seq' in obj:
        fatal('No seq in: %s' % line)

    if not 'field1' in obj:
        fatal('Missing field1 (test_id) in: %s' % line)

    # Ignore messages from wrong test ids
    if str(obj['field1']) != test_id:
        log('Ignoring message for wrong test_id %s, wanted %s' %
            (str(obj['field1']), test_id))
        return

    # Check correct sequence number
    try:
        seq = int(obj['seq'])
    except ValueError:
        fatal('seq %s is not an int' % obj['seq'])

    # First time we see a seq, set up where we expect this to end.
    if o['last_seq'] == 0:
        o['seq_hi'] = seq + (((args.cnt / o['rate'])-seq)*o['rate'])
        log('first seq seen %d, end seq will be %d, rate %d (test_id %s)' % \
                (seq, o['seq_hi'], o['rate'], obj['field1']))

    if o['last_seq'] > 0 and o['last_seq'] + int(o['rate']) != seq:
        fatal('Received seq %d, last received seq %d, expected %d' % \
                  (seq, o['last_seq'], o['last_seq'] + int(o['rate'])))
    o['last_seq'] = seq

    if obj['seq'] != obj['field4']:
        fatal('"seq" and "field4" differs, should be same: %s' % obj)
                            
    # Verify received content
    for f in json_obj:
        if f not in obj:
            fatal('Expected field %s not received: %s' % \
                                (f, obj))

        if f in ('seq', 'field4'):
            continue

        if str(json_obj[f]) != str(obj[f]):
            fatal('Expected field %s value "%s", '
                            'but got "%s" in message: %s' % \
                                (f, json_obj[cf], str(obj[f]), obj))

    if o['last_seq'] == o['seq_hi']:
        log('All seqs seen. We are now good')
        return 1
    elif o['last_seq'] > o['seq_hi']:
        fatal('Overshoot: seq %(last_seq)i > hi %(seq_h)i' % o)

    return 0


"""
Reads from the created fifo and checks that the output matches what
is expected in terms of encoding, sample rate, order, etc.
'o' is the output configuration.
"""
def output_checker_main (o):
    global outputs_started
    outputs_started += 1

    log('opening output checker on ' + o['fifo'])

    f = open(o['fifo'], 'r')

    log('output checker opened fifo %s, sample-rate %d' % \
            (o['fifo'], o['rate']))

    o['last_seq'] = 0;

    line = f.readline()
    cnt = 0
    while line:
        line = line.rstrip()
        cnt += 1
        if (cnt % 1000) == 0:
            log('%i messages read' % cnt)
        #log('READ LINE: ' + o['fifo'] + ': ' + line + ';')
        if check_line(o, line) == 1:
            break
        line = f.readline()

    f.close()
    os.unlink(o['fifo'])

    global outputs_good
    outputs_good += 1
    return


def output_checker_start (o):
    thr = threading.Thread(None, output_checker_main, 'output ' + o['fifo'],
                           (o,))
    thr.daemon = False
    thr.start()
    return thr


def sig_timeout (sig, frame):
    log('\033[31mFATAL: Test timed out\033[0m')
    os.kill(os.getpid(), 9)
    # NOTREACHED
    fatal("Test timed out")
    

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='kafkatee test tool')
    parser.add_argument('--cnt', default=1, type=int, help="Message count")
    parser.add_argument('--genenc', default='string', type=str,
                        help="Input generator encoding")
    parser.add_argument('--checkenc', default='string', type=str,
                        help="Output checker encoding")
    parser.add_argument('--seq', default=1, type=int,
                        help='Start sequence number')
    parser.add_argument('--idle', default=0, type=int,
                        help='Idle time after finishing write (-i)')
    parser.add_argument('--chunk', default=1000, type=int,
                        help='Write chunk size expressed in messages (-i)')
    parser.add_argument('--config', required=True, type=str,
                        help="Configuration in JSON format")
    parser.add_argument('--timeout', default=30, type=int,
                        help='Test timeout')

    # Parse command line arguments
    global args
    args = parser.parse_args()
    conf = json.loads(args.config)

    if 'outputs' not in conf or len(conf['outputs']) == 0:
        fatal('Invalid configuration JSON object: missing outputs')

    args.seq_hi = args.seq + args.cnt

    # Generate unique test_id
    test_id = uuid.uuid1().hex[0:7]
    log('Running test id %s with timeout %ds' % (test_id, args.timeout))

    # Install test timeout alarm
    signal.signal(signal.SIGALRM, sig_timeout)
    signal.alarm(args.timeout)


    # Start input generator thread
    input_generator_start()

    # Start output checker threads
    for o in conf['outputs']:
        o['thread'] = output_checker_start(o)

    # Monitor output checker threads and wait for them to finish.
    while True:
        time.sleep(0.1)
        cnt = 0
        for o in conf['outputs']:
            if o['thread'].is_alive():
                cnt += 1
                o['thread'].join(0.1)
                # If the thread was joined we'll see it on the next run

        if cnt == 0:
            break

    if failures > 0:
        fatal("%i test failures" % failures)
    if outputs_good != outputs_started:
        fatal("%i/%i output checkers failed" % (outputs_started-outputs_good,
                                                outputs_started))
    else:
        log('all %i output checkers finished' % outputs_started)
        sys.exit(0)

