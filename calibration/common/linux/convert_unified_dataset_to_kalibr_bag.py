#!/usr/bin/env python
import argparse
import csv
import os

import cv2
import rosbag
import rospy
from sensor_msgs.msg import Image, Imu


def stamp_from_ns(ns):
    return rospy.Time.from_sec(float(ns) / 1e9)


def make_image_msg(path, timestamp_ns, frame_id):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    if img is None:
        raise RuntimeError("failed to read image: %s" % path)

    msg = Image()
    msg.header.stamp = stamp_from_ns(timestamp_ns)
    msg.header.frame_id = frame_id
    msg.height = int(img.shape[0])
    msg.width = int(img.shape[1])
    msg.encoding = "mono8"
    msg.is_bigendian = 0
    msg.step = int(img.shape[1])
    msg.data = img.tostring()
    return msg


def make_imu_msg(row):
    ts = int(row["timestamp_ns"])

    msg = Imu()
    msg.header.stamp = stamp_from_ns(ts)
    msg.header.frame_id = "imu0"

    msg.angular_velocity.x = float(row["gx"])
    msg.angular_velocity.y = float(row["gy"])
    msg.angular_velocity.z = float(row["gz"])

    msg.linear_acceleration.x = float(row["ax"])
    msg.linear_acceleration.y = float(row["ay"])
    msg.linear_acceleration.z = float(row["az"])

    msg.orientation_covariance[0] = -1.0
    return ts, msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-imu", action="store_true")
    args = ap.parse_args()

    ds = os.path.abspath(args.dataset)
    out = os.path.abspath(args.out)

    out_dir = os.path.dirname(out)
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir)

    camera_rows = []
    with open(os.path.join(ds, "camera_timestamps.csv"), "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            camera_rows.append(row)

    imu_rows = []
    if not args.no_imu:
        with open(os.path.join(ds, "imu.csv"), "r") as f:
            reader = csv.DictReader(f)
            for row in reader:
                imu_rows.append(row)

    events = []

    for row in camera_rows:
        ts = int(row["timestamp_ns"])
        events.append((ts, "cam0", row))
        events.append((ts, "cam1", row))

    for row in imu_rows:
        ts = int(row["timestamp_ns"])
        events.append((ts, "imu", row))

    events.sort(key=lambda x: x[0])

    print("dataset:", ds)
    print("out:", out)
    print("camera pairs:", len(camera_rows))
    print("imu rows:", len(imu_rows))
    print("events:", len(events))

    bag = rosbag.Bag(out, "w")
    try:
        n_cam0 = 0
        n_cam1 = 0
        n_imu = 0

        for ts, kind, row in events:
            t = stamp_from_ns(ts)

            if kind == "cam0":
                img_path = os.path.join(ds, row["cam0_file"])
                msg = make_image_msg(img_path, ts, "cam0")
                bag.write("/cam0/image_raw", msg, t)
                n_cam0 += 1

            elif kind == "cam1":
                img_path = os.path.join(ds, row["cam1_file"])
                msg = make_image_msg(img_path, ts, "cam1")
                bag.write("/cam1/image_raw", msg, t)
                n_cam1 += 1

            else:
                _, msg = make_imu_msg(row)
                bag.write("/imu0", msg, t)
                n_imu += 1

        print("written cam0:", n_cam0)
        print("written cam1:", n_cam1)
        print("written imu:", n_imu)
    finally:
        bag.close()

    print(out)


if __name__ == "__main__":
    main()
