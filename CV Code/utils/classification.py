import cv2 as cv
import numpy as np
from typing import Sequence

from mediapipe.tasks.python.components.containers.landmark import NormalizedLandmark

def draw_hand_landmarks(frame: cv.Mat, hand_landmarks: list[NormalizedLandmark], color=(0, 255, 0)):
    height, width = frame.shape[:2]

    for landmark in hand_landmarks:
        center = (int(landmark.x * width), int(landmark.y * height))
        cv.circle(frame, center, 5, color, -1)

FINGER_JOINTS: tuple[tuple[int, int, int], ...] = (
    (2, 3, 4),      # thumb: mcp, ip, tip
    (5, 6, 7),      # index: mcp, pip, dip
    (9, 10, 11),    # middle
    (13, 14, 15),   # ring
    (17, 18, 19),   # pinky
)

TIP_INDICES = {
    "thumb": 4,
    "index": 8,
    "middle": 12,
    "ring": 16,
    "pinky": 20,
}

MCP_INDICES = {
    "thumb": 2,
    "index": 5,
    "middle": 9,
    "ring": 13,
    "pinky": 17,
}

class ClassificationModel:
    def __init__(self):
        self.open_angle_threshold = 150.0
        self.closed_angle_threshold = 130.0
        self.pointing_index_open_threshold = 145.0
        self.pointing_extension_ratio = 1.24
        self.curled_ratio = 1.18

    def _get_angle_between_points(self, p1: NormalizedLandmark, p2: NormalizedLandmark, p3: NormalizedLandmark) -> float:
        a = np.array([p1.x, p1.y])
        b = np.array([p2.x, p2.y])
        c = np.array([p3.x, p3.y])

        ba = a - b
        bc = c - b

        if np.linalg.norm(ba) == 0 or np.linalg.norm(bc) == 0:
            return 0.0

        cosine_angle = np.dot(ba, bc) / (np.linalg.norm(ba) * np.linalg.norm(bc))
        cosine_angle = float(np.clip(cosine_angle, -1.0, 1.0))
        angle = np.arccos(cosine_angle)

        return np.degrees(angle)
    
    def _is_finger_open(self, finger_points: Sequence[NormalizedLandmark], threshold: float | None = None) -> bool:
        pip_angle = self._get_angle_between_points(
            finger_points[0],  # MCP
            finger_points[1],  # PIP
            finger_points[2]   # DIP
        )
        return pip_angle > (threshold if threshold is not None else self.open_angle_threshold)

    def _is_finger_closed(self, finger_points: Sequence[NormalizedLandmark]) -> bool:
        pip_angle = self._get_angle_between_points(
            finger_points[0],  # MCP
            finger_points[1],  # PIP
            finger_points[2]   # DIP
        )
        return pip_angle < self.closed_angle_threshold

    def _finger_points(self, hand_landmarks: list[NormalizedLandmark], finger: tuple[int, int, int]) -> list[NormalizedLandmark]:
        return [hand_landmarks[i] for i in finger]

    def _distance(self, p1: NormalizedLandmark, p2: NormalizedLandmark) -> float:
        return float(np.hypot(p1.x - p2.x, p1.y - p2.y))

    def _is_extended_from_palm(self, hand_landmarks: list[NormalizedLandmark], finger_name: str, ratio: float) -> bool:
        wrist = hand_landmarks[0]
        mcp = hand_landmarks[MCP_INDICES[finger_name]]
        tip = hand_landmarks[TIP_INDICES[finger_name]]
        return self._distance(tip, wrist) > self._distance(mcp, wrist) * ratio

    def _is_curled_to_palm(self, hand_landmarks: list[NormalizedLandmark], finger_name: str, ratio: float) -> bool:
        wrist = hand_landmarks[0]
        mcp = hand_landmarks[MCP_INDICES[finger_name]]
        tip = hand_landmarks[TIP_INDICES[finger_name]]
        return self._distance(tip, wrist) < self._distance(mcp, wrist) * ratio

    def classify_open_hand(self, hand_landmarks: list[NormalizedLandmark]) -> bool:
        for finger in FINGER_JOINTS:
            if not self._is_finger_open(self._finger_points(hand_landmarks, finger)):
                return False
        
        return True
    
    def classify_fist(self, hand_landmarks: list[NormalizedLandmark]) -> bool:
        curled_non_thumb = 0
        for finger_name, finger_joint in [
            ("index", FINGER_JOINTS[1]),
            ("middle", FINGER_JOINTS[2]),
            ("ring", FINGER_JOINTS[3]),
            ("pinky", FINGER_JOINTS[4]),
        ]:
            by_angle = self._is_finger_closed(self._finger_points(hand_landmarks, finger_joint))
            by_distance = self._is_curled_to_palm(hand_landmarks, finger_name, self.curled_ratio)
            if by_angle or by_distance:
                curled_non_thumb += 1

        index_extended = self._is_extended_from_palm(hand_landmarks, "index", self.pointing_extension_ratio)
        return curled_non_thumb >= 3 and not index_extended
    
    def classify_pointing(self, hand_landmarks: list[NormalizedLandmark]) -> bool:
        index_is_open_angle = self._is_finger_open(
            self._finger_points(hand_landmarks, FINGER_JOINTS[1]),
            threshold=self.pointing_index_open_threshold
        )
        index_is_extended = self._is_extended_from_palm(hand_landmarks, "index", self.pointing_extension_ratio)
        if not (index_is_open_angle or index_is_extended):
            return False

        # For pointing, middle/ring/pinky should be mostly curled. Thumb is optional.
        non_index_fingers = [
            ("middle", FINGER_JOINTS[2]),
            ("ring", FINGER_JOINTS[3]),
            ("pinky", FINGER_JOINTS[4]),
        ]
        closed_count = 0
        for finger_name, finger_joint in non_index_fingers:
            by_angle = self._is_finger_closed(self._finger_points(hand_landmarks, finger_joint))
            by_distance = self._is_curled_to_palm(hand_landmarks, finger_name, self.curled_ratio)
            if by_angle or by_distance:
                closed_count += 1

        # Allow one noisy finger while preserving pointing intent.
        return closed_count >= 2
    
    def classify_pointing_towards_camera(self, hand_landmarks: list[NormalizedLandmark]) -> bool:
        if not self.classify_pointing(hand_landmarks):
            return False
        
        index_tip_z = hand_landmarks[FINGER_JOINTS[1][2]].z
        
        return index_tip_z < -0.1
    
    def classify_pointing_away_from_camera(self, hand_landmarks: list[NormalizedLandmark]) -> bool:
        if not self.classify_pointing(hand_landmarks):
            return False
        
        index_tip_z = hand_landmarks[FINGER_JOINTS[1][2]].z
        
        return index_tip_z > 0.1
    
    def classify_gesture(self, hand_landmarks: list[NormalizedLandmark]) -> str:
        if self.classify_open_hand(hand_landmarks):
            return 'Open Hand'
        elif self.classify_pointing_towards_camera(hand_landmarks):
            return 'Pointing Towards Camera'
        elif self.classify_pointing_away_from_camera(hand_landmarks):
            return 'Pointing Away From Camera'
        elif self.classify_fist(hand_landmarks):
            return 'Fist'
        else:
            return "Unknown Gesture"
