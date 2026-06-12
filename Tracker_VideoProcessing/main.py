# main.py — Motion Tracker Suite v4.0
# Head/upper-body focus: full blendshapes, head Euler angles, upper-body pose
# Install: pip install opencv-python mediapipe websockets numpy

import cv2
import numpy as np
import json
import asyncio
import websockets
import threading
import queue
import time
import urllib.request
import os
import sys
import signal
import math
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision as mp_vision
from mediapipe import Image, ImageFormat

# ─── Connection tables ────────────────────────────────────────────────────────
POSE_CONNECTIONS = [
    (0,1),(1,2),(2,3),(3,7),(0,4),(4,5),(5,6),(6,8),(9,10),
    (11,12),(11,13),(13,15),(15,17),(15,19),(15,21),(17,19),
    (12,14),(14,16),(16,18),(16,20),(16,22),(18,20),
    (11,23),(12,24),(23,24),(23,25),(24,26),
    (25,27),(26,28),(27,29),(28,30),(29,31),(30,32),(27,31),(28,32),
]
HAND_CONNECTIONS = [
    (0,1),(1,2),(2,3),(3,4),(0,5),(5,6),(6,7),(7,8),
    (0,9),(9,10),(10,11),(11,12),(0,13),(13,14),(14,15),(15,16),
    (0,17),(17,18),(18,19),(19,20),(5,9),(9,13),(13,17),
]
FACE_CONTOUR_PAIRS = [
    (10,338),(338,297),(297,332),(332,284),(284,251),(251,389),(389,356),
    (356,454),(454,323),(323,361),(361,288),(288,397),(397,365),(365,379),
    (379,378),(378,400),(400,377),(377,152),(152,148),(148,176),(176,149),
    (149,150),(150,136),(136,172),(172,58),(58,132),(132,93),(93,234),
    (234,127),(127,162),(162,21),(21,54),(54,103),(103,67),(67,109),(109,10),
    (33,7),(7,163),(163,144),(144,145),(145,153),(153,154),(154,155),(155,133),
    (33,246),(246,161),(161,160),(160,159),(159,158),(158,157),(157,173),(173,133),
    (362,382),(382,381),(381,380),(380,374),(374,373),(373,390),(390,249),(249,263),
    (362,466),(466,388),(388,387),(387,386),(386,385),(385,384),(384,398),(398,263),
    (61,146),(146,91),(91,181),(181,84),(84,17),(17,314),(314,405),(405,321),(321,375),(375,291),
    (61,185),(185,40),(40,39),(39,37),(37,0),(0,267),(267,269),(269,270),(270,409),(409,291),
]

MODEL_URLS = {
    "pose": "https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_heavy/float16/latest/pose_landmarker_heavy.task",
    "face": "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/latest/face_landmarker.task",
    "hand": "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task",
}
MODEL_DIR = "models"

def download_model(name, url):
    os.makedirs(MODEL_DIR, exist_ok=True)
    path = os.path.join(MODEL_DIR, f"{name}.task")
    if not os.path.exists(path):
        print(f"Downloading {name} model...")
        urllib.request.urlretrieve(url, path)
    return path

# ─── One-Euro filter ─────────────────────────────────────────────────────────
class OneEuroFilter:
    """
    Casiez et al. 2012 — adaptive low-pass filter.
    v4.0: defaults tuned for head/face tracking responsiveness.
    """
    def __init__(self, min_cutoff: float = 1.0, beta: float = 0.3, d_cutoff: float = 1.0):
        self.min_cutoff = min_cutoff
        self.beta       = beta
        self.d_cutoff   = d_cutoff
        self._x         = None
        self._dx        = 0.0

    @staticmethod
    def _alpha(cutoff: float, freq: float) -> float:
        tau = 1.0 / (2.0 * math.pi * cutoff)
        te  = 1.0 / max(freq, 1.0)
        return 1.0 / (1.0 + tau / te)

    def __call__(self, x: float, freq: float) -> float:
        if self._x is None:
            self._x = x
            return x
        dx       = (x - self._x) * freq
        a_d      = self._alpha(self.d_cutoff, freq)
        self._dx = a_d * dx + (1.0 - a_d) * self._dx
        cutoff   = self.min_cutoff + self.beta * abs(self._dx)
        a        = self._alpha(cutoff, freq)
        self._x  = a * x + (1.0 - a) * self._x
        return self._x

    def reset(self):
        self._x  = None
        self._dx = 0.0


class LandmarkSmoother:
    """Per-coordinate One-Euro smoother for a fixed-size landmark list."""
    def __init__(self, n: int, min_cutoff: float = 1.0, beta: float = 0.3):
        self.filters = [
            [OneEuroFilter(min_cutoff, beta) for _ in range(3)]
            for _ in range(n)
        ]
        self.min_cutoff = min_cutoff
        self.beta       = beta

    def smooth(self, lms: List[Dict], freq: float) -> List[Dict]:
        out = []
        for i, lm in enumerate(lms):
            if i >= len(self.filters):
                out.append(lm); continue
            sx = self.filters[i][0](lm["x"], freq)
            sy = self.filters[i][1](lm["y"], freq)
            sz = self.filters[i][2](lm["z"], freq)
            out.append({**lm, "x": sx, "y": sy, "z": sz})
        return out

    def reset(self):
        for f3 in self.filters:
            for f in f3:
                f.reset()


# ─── Head rotation helper ─────────────────────────────────────────────────────
# Key face landmark indices for computing head orientation
# Using MediaPipe 468-point face mesh canonical landmark IDs
_NOSE_TIP     = 1
_FOREHEAD     = 10
_CHIN         = 152
_LEFT_EAR     = 234
_RIGHT_EAR    = 454
_LEFT_EYE     = 33
_RIGHT_EYE    = 263
_UPPER_LIP    = 13
_LOWER_LIP    = 14

def compute_head_euler(face_lms_world: List[Dict], is_world: bool = False) -> Optional[Dict]:
    """
    Compute head Euler angles (degrees) from world-space (or image-space) face landmarks.
    Works with both metric world-space and normalized image-space coordinates.
    Returns {"yaw": float, "pitch": float, "roll": float} or None.

    Convention (Y-up, right-handed, like VRM):
      yaw   — rotation about Y (left/right, positive = turn right)
      pitch — rotation about X (up/down, positive = look up)
      roll  — rotation about Z (tilt, positive = tilt right)
    """
    if not face_lms_world or len(face_lms_world) < 468:
        return None
    try:
        def p(idx):
            l = face_lms_world[idx]
            return np.array([l["x"], l["y"], l["z"]], dtype=np.float64)

        nose      = p(_NOSE_TIP)      # 1
        forehead  = p(_FOREHEAD)      # 10
        chin      = p(_CHIN)          # 152
        left_ear  = p(_LEFT_EAR)      # 234
        right_ear = p(_RIGHT_EAR)     # 454

        is_image_space = not is_world

        if is_image_space:
            # In image space: y increases downward, x increases rightward from camera
            # face-up direction: forehead has SMALLER y than chin
            up_raw    = chin - forehead        # points downward in image = face-down; invert later
            right_raw = left_ear - right_ear   # face-right from subject's perspective = camera-left = smaller x
        else:
            # World space: Y up
            up_raw    = forehead - chin
            right_raw = left_ear - right_ear

        up_n    = np.linalg.norm(up_raw)
        right_n = np.linalg.norm(right_raw)
        if up_n < 1e-6 or right_n < 1e-6:
            return None

        up    = up_raw    / up_n
        right = right_raw / right_n

        # Re-orthogonalise
        fwd   = np.cross(right, up)
        fwd_n = np.linalg.norm(fwd)
        if fwd_n < 1e-6:
            return None
        fwd   = fwd / fwd_n
        right = np.cross(up, fwd)
        right_n2 = np.linalg.norm(right)
        if right_n2 < 1e-6:
            return None
        right = right / right_n2

        if is_image_space:
            # Flip axes to get VRM Y-up convention from image space:
            # image up = -up (since we used chin-forehead), image right-from-subject = right
            up    = -up
            fwd   = -fwd

        # Build 3x3 rotation matrix (columns: right, up, fwd)
        R = np.column_stack([right, up, fwd])

        # Extract Euler angles
        pitch = math.degrees(math.asin(float(np.clip(-R[2, 1], -1.0, 1.0))))
        yaw   = math.degrees(math.atan2(float(R[2, 0]), float(R[2, 2])))
        roll  = math.degrees(math.atan2(float(R[0, 1]), float(R[1, 1])))

        # Shift yaw by 180 degrees to align camera-facing tracker space with model space
        yaw = yaw - 180.0 if yaw > 0.0 else yaw + 180.0
        if yaw < -180.0: yaw += 360.0
        elif yaw > 180.0: yaw -= 360.0

        return {"yaw": round(yaw, 3), "pitch": round(pitch, 3), "roll": round(roll, 3)}
    except Exception as e:
        print(f"[head_euler] error: {e}")
        return None


def smooth_euler(
        euler: Optional[Dict],
        yaw_f: OneEuroFilter, pitch_f: OneEuroFilter, roll_f: OneEuroFilter,
        freq: float
) -> Optional[Dict]:
    if euler is None:
        return None
    return {
        "yaw":   yaw_f(euler["yaw"],   freq),
        "pitch": pitch_f(euler["pitch"], freq),
        "roll":  roll_f(euler["roll"],  freq),
    }


# ─── Settings (shared between main thread and WS thread) ─────────────────────
class TrackerSettings:
    def __init__(self):
        self.infer_w        = 480
        self.infer_h        = 270
        # One-Euro params — v4.0 tuned for head/face responsiveness
        self.pose_min_cutoff  = 1.2
        self.pose_beta        = 0.25
        self.face_min_cutoff  = 1.0
        self.face_beta        = 0.30
        self.hand_min_cutoff  = 1.5
        self.hand_beta        = 0.20
        # Euler angle smoothing
        self.euler_min_cutoff = 2.0
        self.euler_beta       = 0.40
        # Visibility threshold for pose landmark gating
        self.pose_vis_threshold = 0.60
        self.camera_index     = 0
        self.reload_smoothers = False   # set True when params change

    def to_json(self) -> str:
        return json.dumps({
            "infer_w":             self.infer_w,
            "infer_h":             self.infer_h,
            "pose_min_cutoff":     self.pose_min_cutoff,
            "pose_beta":           self.pose_beta,
            "face_min_cutoff":     self.face_min_cutoff,
            "face_beta":           self.face_beta,
            "hand_min_cutoff":     self.hand_min_cutoff,
            "hand_beta":           self.hand_beta,
            "euler_min_cutoff":    self.euler_min_cutoff,
            "euler_beta":          self.euler_beta,
            "pose_vis_threshold":  self.pose_vis_threshold,
        }, indent=2)

    def save(self, path="tracker_settings.json"):
        with open(path, "w") as f: f.write(self.to_json())

    def load(self, path="tracker_settings.json"):
        if not os.path.exists(path): return
        try:
            d = json.load(open(path))
            for k, v in d.items():
                if hasattr(self, k): setattr(self, k, v)
            print(f"Settings loaded from {path}")
        except Exception as e:
            print(f"Settings load failed: {e}")


# ─── Worker thread ────────────────────────────────────────────────────────────
class ModelWorker(threading.Thread):
    def __init__(self, name, landmarker, in_q, out_q):
        super().__init__(daemon=True, name=name)
        self.landmarker = landmarker
        self.in_q  = in_q
        self.out_q = out_q
        self._running = True

    def run(self):
        while self._running:
            try:
                mp_img, ts_ms = self.in_q.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                result = self.landmarker.detect_for_video(mp_img, ts_ms)
                # Drain stale output, keep only latest
                while not self.out_q.empty():
                    try: self.out_q.get_nowait()
                    except: pass
                self.out_q.put(result)
            except Exception as e:
                print(f"[{self.name}] error: {e}")

    def stop(self): self._running = False


# ─── Tracker ──────────────────────────────────────────────────────────────────
def is_parent_alive(pid: int) -> bool:
    if pid is None:
        return True
    try:
        if os.name == 'nt':
            import ctypes
            SYNCHRONIZE = 0x00100000
            kernel32 = ctypes.windll.kernel32
            handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
            if handle:
                res = kernel32.WaitForSingleObject(handle, 0)
                kernel32.CloseHandle(handle)
                return res == 0x102  # WAIT_TIMEOUT (still running)
            return False
        else:
            os.kill(pid, 0)
            return True
    except OSError:
        return False
    except Exception:
        return False


class MotionTracker:
    def __init__(self, settings: TrackerSettings):
        self.settings = settings
        settings.load()

        try:
            base = mp_python.BaseOptions
            pose_lm = mp_vision.PoseLandmarker.create_from_options(
                mp_vision.PoseLandmarkerOptions(
                    base_options=base(model_asset_path=download_model("pose", MODEL_URLS["pose"])),
                    running_mode=mp_vision.RunningMode.VIDEO,
                    num_poses=1,
                    min_pose_detection_confidence=0.5,
                    min_pose_presence_confidence=0.5,
                    min_tracking_confidence=0.5,
                    output_segmentation_masks=False,
                ))
            face_lm = mp_vision.FaceLandmarker.create_from_options(
                mp_vision.FaceLandmarkerOptions(
                    base_options=base(model_asset_path=download_model("face", MODEL_URLS["face"])),
                    running_mode=mp_vision.RunningMode.VIDEO,
                    num_faces=1,
                    min_face_detection_confidence=0.5,
                    min_face_presence_confidence=0.5,
                    min_tracking_confidence=0.5,
                    output_face_blendshapes=True,
                ))
            hand_lm = mp_vision.HandLandmarker.create_from_options(
                mp_vision.HandLandmarkerOptions(
                    base_options=base(model_asset_path=download_model("hand", MODEL_URLS["hand"])),
                    running_mode=mp_vision.RunningMode.VIDEO,
                    num_hands=2,
                    min_hand_detection_confidence=0.5,
                    min_hand_presence_confidence=0.5,
                    min_tracking_confidence=0.5,
                ))
        except Exception as e:
            print(f"Model init failed: {e}"); sys.exit(1)

        self.pose_in  = queue.Queue(maxsize=1)
        self.face_in  = queue.Queue(maxsize=1)
        self.hand_in  = queue.Queue(maxsize=1)
        self.pose_out = queue.Queue(maxsize=1)
        self.face_out = queue.Queue(maxsize=1)
        self.hand_out = queue.Queue(maxsize=1)

        self.workers = [
            ModelWorker("pose", pose_lm, self.pose_in,  self.pose_out),
            ModelWorker("face", face_lm, self.face_in,  self.face_out),
            ModelWorker("hand", hand_lm, self.hand_in,  self.hand_out),
        ]

        self._init_smoothers()

        self.last_pose_r = None
        self.last_face_r = None
        self.last_hand_r = None

        self.cap = cv2.VideoCapture(self.settings.camera_index)
        if not self.cap.isOpened():
            print(f"Cannot open camera {self.settings.camera_index}"); sys.exit(1)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH,  1280)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

        self.preview_enabled = False
        self.window_name     = "Motion Tracker v4.0 | P=preview  Q=quit"
        self.latest_payload  = None
        self.latest_frame_id = 0
        self.running         = True

        # Parse parent pid if present
        self.parent_pid = None
        for i in range(len(sys.argv) - 1):
            if sys.argv[i] == "--parent-pid":
                try:
                    self.parent_pid = int(sys.argv[i+1])
                    print(f"Tracking parent process PID: {self.parent_pid}")
                except Exception as e:
                    print(f"Error parsing parent PID: {e}")

        # Monotonic start time for ts_ms
        self._start_perf = time.perf_counter()

        signal.signal(signal.SIGINT,  self._sig)
        signal.signal(signal.SIGTERM, self._sig)

    def _sig(self, *_): self.running = False

    def _init_smoothers(self):
        s = self.settings
        self.pose_smoother = None   # lazy — need landmark count first
        self.face_smoother = None
        self.lh_smoother   = None
        self.rh_smoother   = None
        # Euler angle smoothers (3 scalars, not landmark arrays)
        self.euler_yaw_f   = OneEuroFilter(s.euler_min_cutoff, s.euler_beta)
        self.euler_pitch_f = OneEuroFilter(s.euler_min_cutoff, s.euler_beta)
        self.euler_roll_f  = OneEuroFilter(s.euler_min_cutoff, s.euler_beta)
        self._smoother_params = (s.pose_min_cutoff, s.pose_beta,
                                 s.face_min_cutoff, s.face_beta,
                                 s.hand_min_cutoff, s.hand_beta,
                                 s.euler_min_cutoff, s.euler_beta)

    def _check_reload_smoothers(self):
        s = self.settings
        cur = (s.pose_min_cutoff, s.pose_beta,
               s.face_min_cutoff, s.face_beta,
               s.hand_min_cutoff, s.hand_beta,
               s.euler_min_cutoff, s.euler_beta)
        if s.reload_smoothers or cur != self._smoother_params:
            self._init_smoothers()
            s.reload_smoothers = False

    # ── Landmark helpers ──────────────────────────────────────────────────────

    def _lms_to_dicts(self, lms):
        if not lms: return None
        out = []
        for lm in lms:
            try:
                vis = float(lm.visibility) if hasattr(lm, 'visibility') and lm.visibility is not None else 1.0
                out.append({"x": float(lm.x), "y": float(lm.y), "z": float(lm.z), "visibility": vis})
            except: continue
        return out or None

    def _world_lms_to_dicts(self, lms):
        return self._lms_to_dicts(lms)

    def _hand_world_anchored(self, hand_lms, pose_world_lms, wrist_idx, elbow_idx):
        if not hand_lms or not pose_world_lms: return self._lms_to_dicts(hand_lms)
        if wrist_idx >= len(pose_world_lms) or elbow_idx >= len(pose_world_lms):
            return self._lms_to_dicts(hand_lms)
        try:
            pw = pose_world_lms[wrist_idx]
            pe = pose_world_lms[elbow_idx]
            wrist_w  = np.array([pw.x, pw.y, pw.z], dtype=np.float32)
            elbow_w  = np.array([pe.x, pe.y, pe.z], dtype=np.float32)
            forearm  = float(np.linalg.norm(wrist_w - elbow_w))

            hw   = np.array([hand_lms[0].x,  hand_lms[0].y,  hand_lms[0].z],  dtype=np.float32)
            hmcp = np.array([hand_lms[5].x,  hand_lms[5].y,  hand_lms[5].z],  dtype=np.float32)
            span = float(np.linalg.norm(hmcp - hw))

            scale = (forearm * 0.35) / max(span, 1e-5)
            scale = float(np.clip(scale, 0.3, 4.0))

            out = []
            for lm in hand_lms:
                dx = (lm.x - hand_lms[0].x) * scale
                dy = -(lm.y - hand_lms[0].y) * scale
                dz = (lm.z - hand_lms[0].z) * scale
                out.append({
                    "x": float(wrist_w[0] + dx),
                    "y": float(wrist_w[1] + dy),
                    "z": float(wrist_w[2] + dz),
                    "visibility": 1.0
                })
            return out or None
        except: return self._lms_to_dicts(hand_lms)

    def _face_world_anchored(self, face_r, pose_world_lms):
        if not face_r or not face_r.face_landmarks:
            return None
        try:
            # Check if we can use MediaPipe's native face_world_landmarks (un-squished, Y-up)
            if hasattr(face_r, 'face_world_landmarks') and face_r.face_world_landmarks:
                face_world_lms = face_r.face_world_landmarks[0]
                if len(face_world_lms) < 2:
                    return None
                pose_nose = np.array([0.0, 0.0, 0.0])
                if pose_world_lms and len(pose_world_lms) > 0:
                    pw = pose_world_lms[0]
                    pose_nose = np.array([pw.x, pw.y, pw.z])
                fl = face_world_lms[1]  # Nose tip in face mesh
                face_nose = np.array([fl.x, fl.y, fl.z])

                out = []
                for lm in face_world_lms:
                    # Negate X relative to face_nose to match standard normalized X-right direction (which C++ expects to invert)
                    dx = -(lm.x - face_nose[0])
                    # Y is Y-up internally in Python (to keep compute_head_euler happy)
                    dy = lm.y - face_nose[1]
                    dz = lm.z - face_nose[2]
                    out.append({
                        "x": float(pose_nose[0] + dx),
                        "y": float(pose_nose[1] + dy),
                        "z": float(pose_nose[2] + dz),
                        "visibility": 1.0
                    })
                return out

            face_lms = face_r.face_landmarks[0]
            if not pose_world_lms or len(pose_world_lms) < 13:
                return self._lms_to_dicts(face_lms)
            
            if len(face_lms) < 153:
                return self._lms_to_dicts(face_lms)

            nose_w = np.array([pose_world_lms[0].x, pose_world_lms[0].y, pose_world_lms[0].z], dtype=np.float32)
            ls_w = np.array([pose_world_lms[11].x, pose_world_lms[11].y, pose_world_lms[11].z], dtype=np.float32)
            rs_w = np.array([pose_world_lms[12].x, pose_world_lms[12].y, pose_world_lms[12].z], dtype=np.float32)
            shoulder_dist = float(np.linalg.norm(ls_w - rs_w))

            fn_pos = np.array([face_lms[1].x, face_lms[1].y, face_lms[1].z], dtype=np.float32)
            ff_xy = np.array([face_lms[10].x, face_lms[10].y], dtype=np.float32)
            fc_xy = np.array([face_lms[152].x, face_lms[152].y], dtype=np.float32)
            face_h = float(np.linalg.norm(ff_xy - fc_xy))

            scale_xy = (shoulder_dist * 0.75) / max(face_h, 1e-5)
            scale_xy = float(np.clip(scale_xy, 0.2, 3.0))

            # Maintain aspect ratio to prevent squishing
            scale_z = scale_xy

            out = []
            for lm in face_lms:
                dx = (lm.x - fn_pos[0]) * scale_xy
                dy = -(lm.y - fn_pos[1]) * scale_xy  # Invert Y to make it Y-up internally
                dz = (lm.z - fn_pos[2]) * scale_z
                out.append({
                    "x": float(nose_w[0] + dx),
                    "y": float(nose_w[1] + dy),
                    "z": float(nose_w[2] + dz),
                    "visibility": 1.0
                })
            return out or None
        except Exception as e:
            print(f"Error in _face_world_anchored: {e}")
            return self._lms_to_dicts(face_r.face_landmarks[0]) if face_r.face_landmarks else None

    def _extract_blink(self, face_r):
        if not face_r or not face_r.face_blendshapes: return 0.0
        left = right = 0.0
        try:
            for bs in face_r.face_blendshapes[0]:
                if   bs.category_name == "eyeBlinkLeft":  left  = bs.score
                elif bs.category_name == "eyeBlinkRight": right = bs.score
        except: pass

        def remap_blink(val):
            threshold = 0.18
            if val < threshold:
                return 0.0
            return (val - threshold) / (1.0 - threshold)

        return (remap_blink(left) + remap_blink(right)) / 2.0

    def _extract_all_blendshapes(self, face_r) -> Optional[Dict[str, float]]:
        """Extract the full ARKit-style blendshape dict from MediaPipe."""
        if not face_r or not face_r.face_blendshapes:
            return None
        try:
            out = {}
            for bs in face_r.face_blendshapes[0]:
                name = bs.category_name
                val = float(bs.score)
                if name in ["eyeBlinkLeft", "eyeBlinkRight"]:
                    threshold = 0.18
                    if val < threshold:
                        val = 0.0
                    else:
                        val = (val - threshold) / (1.0 - threshold)
                out[name] = round(val, 5)
            return out if out else None
        except:
            return None

    def _gate_pose_landmarks(self, lms: List[Dict], vis_threshold: float) -> List[Dict]:
        """Pass through landmarks with original visibility so C++ can filter them dynamically."""
        return lms

    # ── Preview ───────────────────────────────────────────────────────────────

    def _draw_connections(self, img, lms, pairs, color, thickness=1):
        h, w = img.shape[:2]
        pts = [(int(lm.x * w), int(lm.y * h)) for lm in lms]
        for a, b in pairs:
            if a < len(pts) and b < len(pts):
                try: cv2.line(img, pts[a], pts[b], color, thickness, cv2.LINE_AA)
                except: pass

    def _draw_points(self, img, lms, color, r=3):
        h, w = img.shape[:2]
        for lm in lms:
            try: cv2.circle(img, (int(lm.x * w), int(lm.y * h)), r, color, -1, cv2.LINE_AA)
            except: pass

    def draw_preview(self, frame, pose_r, face_r, hand_r, blink, fps, euler):
        out = frame.copy()
        h, w = out.shape[:2]
        s = self.settings
        if pose_r and pose_r.pose_landmarks:
            self._draw_connections(out, pose_r.pose_landmarks[0], POSE_CONNECTIONS, (50,220,50), 2)
            self._draw_points(out, pose_r.pose_landmarks[0], (255,255,255), 3)
        if face_r and face_r.face_landmarks:
            self._draw_connections(out, face_r.face_landmarks[0], FACE_CONTOUR_PAIRS, (0,220,220), 1)
        if hand_r and hand_r.hand_landmarks:
            for i, hl in enumerate(hand_r.hand_landmarks):
                c = (0,100,255) if (i < len(hand_r.handedness) and
                                    hand_r.handedness[i][0].category_name == "Left") else (0,165,255)
                self._draw_connections(out, hl, HAND_CONNECTIONS, c, 2)
                self._draw_points(out, hl, (0,230,255), 4)
        ov = out.copy()
        cv2.rectangle(ov, (8, 8), (420, 150), (0,0,0), -1)
        cv2.addWeighted(ov, 0.55, out, 0.45, 0, out)
        bc = (80,255,80) if blink < 0.35 else (60,60,255)
        cv2.putText(out, f"Blink: {blink:.2f}",                          (18, 38), cv2.FONT_HERSHEY_SIMPLEX, 0.65, bc,           2, cv2.LINE_AA)
        cv2.putText(out, f"FPS:   {fps:.1f}",                            (18, 68), cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0,220,255),  2, cv2.LINE_AA)
        if euler:
            cv2.putText(out, f"Y={euler['yaw']:+6.1f} P={euler['pitch']:+6.1f} R={euler['roll']:+6.1f}",
                        (18, 98), cv2.FONT_HERSHEY_SIMPLEX, 0.50, (255,180,80), 1, cv2.LINE_AA)
        cv2.putText(out, f"Infer: {s.infer_w}x{s.infer_h}",             (18, 126), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (160,160,160), 1, cv2.LINE_AA)
        cv2.putText(out, "P=preview  Q=quit", (10, h-12),                          cv2.FONT_HERSHEY_SIMPLEX, 0.5,  (180,180,180), 1, cv2.LINE_AA)
        return out

    # ── Build payload ─────────────────────────────────────────────────────────

    def _build_data(self, ts_ms, fps):
        self._check_reload_smoothers()
        s = self.settings

        pose_r = self.last_pose_r
        face_r = self.last_face_r
        hand_r = self.last_hand_r

        pose_lms = pose_world = face_world = left_hand = right_hand = None
        head_euler_out = None
        blendshapes_out = None

        # ── Pose world landmarks ──────────────────────────────────────────────
        pose_wlms_raw = None   # raw MediaPipe pose world landmark list (for anchoring)
        if pose_r and pose_r.pose_landmarks:
            pose_lms = self._lms_to_dicts(pose_r.pose_landmarks[0])
            if pose_r.pose_world_landmarks:
                raw = self._world_lms_to_dicts(pose_r.pose_world_landmarks[0])
                if raw:
                    if self.pose_smoother is None:
                        self.pose_smoother = LandmarkSmoother(len(raw), s.pose_min_cutoff, s.pose_beta)
                    smoothed = self.pose_smoother.smooth(raw, fps)
                    # Gate by visibility threshold, but always include data (even low-vis)
                    pose_world = self._gate_pose_landmarks(smoothed, s.pose_vis_threshold)
                    pose_wlms_raw = pose_r.pose_world_landmarks[0]  # save for anchoring

        # ── Face world landmarks + head euler ─────────────────────────────────
        if face_r and face_r.face_landmarks:
            face_lms_raw = face_r.face_landmarks[0]
            # Try world-anchored (metric) if pose is available
            raw = self._face_world_anchored(face_r, pose_wlms_raw)
            if not raw:
                # Fallback: use image-space face landmarks directly
                raw = self._lms_to_dicts(face_lms_raw)

            if raw:
                if self.face_smoother is None or len(self.face_smoother.filters) != len(raw):
                    self.face_smoother = LandmarkSmoother(len(raw), s.face_min_cutoff, s.face_beta)
                face_world = self.face_smoother.smooth(raw, fps)

                is_world_mode = (pose_wlms_raw is not None)
                raw_euler = compute_head_euler(face_world, is_world=is_world_mode)
                if raw_euler is None:
                    # Fallback: try directly from raw (unsmoothed)
                    raw_euler = compute_head_euler(raw, is_world=is_world_mode)
                head_euler_out = smooth_euler(
                    raw_euler,
                    self.euler_yaw_f, self.euler_pitch_f, self.euler_roll_f,
                    fps
                )
                if head_euler_out is None and raw_euler is not None:
                    head_euler_out = raw_euler  # use unsmoothed if filter fails

        # ── Extract full blendshapes ──────────────────────────────────────────
        blendshapes_out = self._extract_all_blendshapes(face_r)

        # ── Hand landmarks (anchored to pose wrist position if pose is available) ──────────────────
        if hand_r and hand_r.hand_landmarks:
            has_pose = pose_r and pose_r.pose_world_landmarks and len(pose_r.pose_world_landmarks[0]) > 16
            pwl = pose_r.pose_world_landmarks[0] if has_pose else None
            for i, hl in enumerate(hand_r.hand_landmarks):
                if i >= len(hand_r.handedness): continue
                label = hand_r.handedness[i][0].category_name
                if label == "Left":
                    raw = self._hand_world_anchored(hl, pwl, 15, 13) if has_pose else self._lms_to_dicts(hl)
                    if raw:
                        if self.lh_smoother is None or len(self.lh_smoother.filters) != len(raw):
                            self.lh_smoother = LandmarkSmoother(len(raw), s.hand_min_cutoff, s.hand_beta)
                        left_hand = self.lh_smoother.smooth(raw, fps)
                else:
                    raw = self._hand_world_anchored(hl, pwl, 16, 14) if has_pose else self._lms_to_dicts(hl)
                    if raw:
                        if self.rh_smoother is None or len(self.rh_smoother.filters) != len(raw):
                            self.rh_smoother = LandmarkSmoother(len(raw), s.hand_min_cutoff, s.hand_beta)
                        right_hand = self.rh_smoother.smooth(raw, fps)

        # ── Debug: print occasionally ─────────────────────────────────────────
        if not hasattr(self, '_dbg_counter'): self._dbg_counter = 0
        self._dbg_counter += 1
        if self._dbg_counter % 300 == 0:
            print(f"[DBG] face={'yes' if face_r and face_r.face_landmarks else 'no'}  "
                  f"pose={'yes' if pose_r and pose_r.pose_landmarks else 'no'}  "
                  f"head_euler={head_euler_out}  "
                  f"face_world_len={len(face_world) if face_world else 0}")

        return {
            "timestamp":   ts_ms / 1000.0,
            "pose":        pose_lms,
            "pose_world":  [{**lm, "y": -lm["y"]} for lm in pose_world] if pose_world else None,
            "face_world":  [{**lm, "y": -lm["y"]} for lm in face_world] if face_world else None,
            "left_hand":   [{**lm, "y": -lm["y"]} for lm in left_hand] if left_hand else None,
            "right_hand":  [{**lm, "y": -lm["y"]} for lm in right_hand] if right_hand else None,
            "blink":       self._extract_blink(face_r),
            "head_euler":  head_euler_out,
            "blendshapes": blendshapes_out,
            "fps":         fps,
        }

    # ── WebSocket ─────────────────────────────────────────────────────────────

    async def _ws_handler(self, websocket, path=None):
        # Reject non-local connections
        remote_ip = websocket.remote_address[0]
        if remote_ip not in ("127.0.0.1", "::1", "localhost"):
            print(f"Connection rejected from unauthorized IP: {remote_ip}")
            await websocket.close(code=4000, reason="Localhost connection only")
            return

        print("Renderer connected")
        last_id = -1
        try:
            while self.running:
                if self.latest_payload is not None and self.latest_frame_id != last_id:
                    await websocket.send(self.latest_payload)
                    last_id = self.latest_frame_id
                await asyncio.sleep(0.001)
        except Exception as e:
            print(f"Renderer disconnected: {e}. Shutting down tracker.")
            self.running = False

    def _start_ws_thread(self):
        async def _serve():
            port = 8765
            server = None
            while port <= 8780:
                try:
                    server = await websockets.serve(self._ws_handler, "localhost", port, ping_interval=None)
                    print(f"WebSocket server started on ws://localhost:{port}")
                    try:
                        with open("tracker_port.txt", "w") as f:
                            f.write(str(port))
                    except Exception as e:
                        print(f"Failed to write tracker_port.txt: {e}")
                    break
                except OSError:
                    port += 1
            if server is None:
                print("Could not bind to any port in range 8765-8780.")
                self.running = False
                return
            await asyncio.Future()
        def _run():
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            loop.run_until_complete(_serve())
            loop.close()
        threading.Thread(target=_run, daemon=True).start()
        print("WebSocket server started on ws://localhost:8765")

    # ── Main loop ─────────────────────────────────────────────────────────────

    def run(self):
        W = int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        H = int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        print(f"\n{'='*56}")
        print("  MOTION TRACKER SUITE v4.0  (Head/Upper-Body Focus)")
        print(f"{'='*56}")
        print(f"  Camera {W}x{H}  ->  infer {self.settings.infer_w}x{self.settings.infer_h}")
        print(f"{'='*56}\n")

        for w in self.workers: w.start()
        self._start_ws_thread()

        fps_counter   = 0
        fps_display   = 30.0
        last_fps_time = time.perf_counter()
        fps_history   = [30.0] * 10

        parent_check_counter = 0

        while self.running and self.cap.isOpened():
            if self.parent_pid is not None:
                parent_check_counter += 1
                if parent_check_counter >= 60:
                    parent_check_counter = 0
                    if not is_parent_alive(self.parent_pid):
                        print(f"Parent process {self.parent_pid} has exited. Killing tracker.")
                        self.running = False
                        break

            ok, frame = self.cap.read()
            if not ok: print("Camera read failed"); break

            ts_ms = int((time.perf_counter() - self._start_perf) * 1000)
            s = self.settings

            small  = cv2.resize(frame, (s.infer_w, s.infer_h), interpolation=cv2.INTER_LINEAR)
            rgb    = cv2.cvtColor(small, cv2.COLOR_BGR2RGB)
            mp_img = Image(image_format=ImageFormat.SRGB, data=rgb)

            for q in (self.pose_in, self.face_in, self.hand_in):
                if q.full():
                    try: q.get_nowait()
                    except: pass
                try: q.put_nowait((mp_img, ts_ms))
                except: pass

            try: self.last_pose_r = self.pose_out.get_nowait()
            except queue.Empty: pass
            try: self.last_face_r = self.face_out.get_nowait()
            except queue.Empty: pass
            try: self.last_hand_r = self.hand_out.get_nowait()
            except queue.Empty: pass

            fps_counter += 1
            now = time.perf_counter()
            if now - last_fps_time >= 0.5:
                measured      = fps_counter / (now - last_fps_time)
                fps_history   = fps_history[1:] + [measured]
                fps_display   = sum(fps_history) / len(fps_history)
                fps_counter   = 0
                last_fps_time = now

            data = self._build_data(ts_ms, fps_display)
            self.latest_payload  = json.dumps(data, allow_nan=False)
            self.latest_frame_id += 1

            if self.preview_enabled and self.last_pose_r is not None:
                preview = self.draw_preview(frame, self.last_pose_r, self.last_face_r,
                                            self.last_hand_r, data["blink"], fps_display,
                                            data.get("head_euler"))
                cv2.imshow(self.window_name, preview)

            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'): break
            elif key == ord('p'):
                self.preview_enabled = not self.preview_enabled
                if not self.preview_enabled: cv2.destroyWindow(self.window_name)

        for w in self.workers: w.stop()
        self.settings.save()
        self.cap.release()
        cv2.destroyAllWindows()
        print("Tracker stopped — settings saved")


if __name__ == "__main__":
    try:
        settings = TrackerSettings()
        MotionTracker(settings).run()
    except KeyboardInterrupt:
        print("\nInterrupted")
    except Exception as e:
        print(f"\nFatal: {e}")
        sys.exit(1)