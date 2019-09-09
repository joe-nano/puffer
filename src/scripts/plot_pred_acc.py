#!/usr/bin/env python3

import os
import sys
import argparse
import yaml
import json
import numpy as np

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

from helpers import connect_to_influxdb
from stream_processor import VideoStreamCallback
from ttp import Model, PKT_BYTES
from ttp2 import Model as Linear_Model

VIDEO_DURATION = 180180

args = None
expt = {}
influx_client = None
ttp_model = Model()
linear_model = Linear_Model()
result = {'ttp': {'bin': 0, 'l1': 0},
        'linear': {'bin': 0, 'l1': 0},
        'harmonic': {'bin': 0, 'l1': 0}}
tot = 0


def discretize_output(raw_out):
    z = np.array(raw_out)

    z = np.floor((z + 0.5 * Model.BIN_SIZE) / Model.BIN_SIZE).astype(int)
    return np.clip(z, 0, Model.BIN_MAX)


def prepare_intput_output(chunks):
    row = []
    for chunk in chunks[1:]:
        row = [chunk['delivery_rate'] / PKT_BYTES, chunk['cwnd'],
               chunk['in_flight'], chunk['min_rtt'], chunk['rtt'],
               chunk['size'] / PKT_BYTES, chunk['trans_time']] + row
    row += [chunks[0]['delivery_rate'] / PKT_BYTES, chunks[0]['cwnd'],
            chunks[0]['in_flight'], chunks[0]['min_rtt'],
            chunks[0]['rtt'], chunks[0]['size'] / PKT_BYTES]

    return ([row], [chunks[0]['trans_time']])

def model_pred(model, raw_in):
    input_data = model.normalize_input(raw_in, update_obs=False)
    model.set_model_eval()

    return model.predict_distr(input_data)

def distr_bin_pred(distr):
    max_bin = np.argmax(distr, axis=1)
    ret = []
    for bin_id in max_bin:
        if bin_id == 0:  # the first bin is defined differently
            ret.append(0.25 * Model.BIN_SIZE)
        else:
            ret.append(bin_id * Model.BIN_SIZE)

    return ret

def distr_l1_pred(distr):
    ret = []
    for dis in distr:
        cnt = 0
        for i in range(len(dis)):
            cnt += dis[i]
            if cnt > 0.5:
                break

        if i == 0:
            ret.append(0.5 * Model.BIN_SIZE / cnt * 0.5)
        else:
            tmp = 0.5 - cnt + 0.5 * dis[i]
            ret.append((i + tmp / dis[i]) * Model.BIN_SIZE)

    return ret

def bin_acc(y, _y):
    return  discretize_output(y) == discretize_output(_y)

def l1_loss(y, _y):
    return np.abs(y - _y)

def harmonic_pred(chunks):
    prev_trans = 0

    for chunk in chunks[1:]:
        prev_trans += chunk['trans_time'] / chunk['size']
    ave_trans = prev_trans / (len(chunks) - 1)

    return [chunks[0]['size'] * ave_trans]

def process_session(s):
    global tot
    global result
    global ttp_model
    global linear_model
    global expt

    past_chunks = ttp_model.PAST_CHUNKS

    for curr_ts in s:
        chunks = []

        for i in reversed(range(past_chunks + 1)):
            ts = curr_ts - i * VIDEO_DURATION
            if ts not in s:
                break
            chunks.append(s[ts])

        if len(chunks) != past_chunks + 1:
            continue

        tot += 1
        (raw_in, raw_out) = prepare_intput_output(chunks)
        ttp_distr = model_pred(ttp_model, raw_in)
        linear_distr = model_pred(linear_model, raw_in)

        # ttp
        bin_ttp_out = distr_bin_pred(ttp_distr)
        l1_ttp_out = distr_l1_pred(ttp_distr)

        result['ttp']['bin'] += bin_acc(bin_ttp_out[0], raw_out[0])
        result['ttp']['l1'] += l1_loss(l1_ttp_out[0], raw_out[0])

        # linear
        bin_linear_out = distr_bin_pred(linear_distr)
        l1_linear_out = distr_l1_pred(linear_distr)

        result['linear']['bin'] += bin_acc(bin_linear_out[0], raw_out[0])
        result['linear']['l1'] += l1_loss(l1_linear_out[0], raw_out[0])

        # harmonic
        harm_out = harmonic_pred(chunks)

        result['harmonic']['bin'] += bin_acc(harm_out[0], raw_out[0])
        result['harmonic']['l1'] += l1_loss(harm_out[0], raw_out[0])

        if tot % 1000 == 0:
            print('For tot:', tot)
            for pred in ['ttp', 'linear', 'harmonic']:
                for term in ['bin', 'l1']:
                    print(pred + ' ' + term + ': {:.5f}'.format(
                          result[pred][term] / tot))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('yaml_settings')
    parser.add_argument('--from', dest='start_time', required=True,
                        help='datetime in UTC conforming to RFC3339')
    parser.add_argument('--to', dest='end_time', required=True,
                        help='datetime in UTC conforming to RFC3339')
    parser.add_argument('--expt', help='e.g., expt_cache.json')
    parser.add_argument('-o', '--output', required=True)
    parser.add_argument('--ttp-path', dest='ttp_path', required=True)
    global args
    args = parser.parse_args()

    with open(args.yaml_settings, 'r') as fh:
        yaml_settings = yaml.safe_load(fh)

    with open(args.expt, 'r') as fh:
        global expt
        expt = json.load(fh)

    # create an InfluxDB client and perform queries
    global influx_client
    influx_client = connect_to_influxdb(yaml_settings)

    global ttp_model
    ttp_model.load(args.ttp_path)

    global linear_model
    linear_model.load("/home/ubuntu/models/puffer_ttp/linear/py-0.pt")

    video_stream = VideoStreamCallback(process_session)
    video_stream.process(influx_client, args.start_time, args.end_time)

    with open(args.output, 'w') as fh:
        for term in ['bin', 'l1']:
            fh.write(term + ':')
            for pred in ['linear', 'ttp', 'harmonic']:
                fh.write(pred + ': {:.5f}, '.format(
                      result[pred][term] / tot))
            fh.write('\n')

if __name__ == '__main__':
    main()
