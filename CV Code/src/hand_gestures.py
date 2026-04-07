from typing import Any

import cv2 as cv
import mediapipe as mp

from utils.classification import ClassificationModel, draw_hand_landmarks

BaseOptions = mp.tasks.BaseOptions
GestureRecognizer = mp.tasks.vision.GestureRecognizer
GestureRecognizerOptions = mp.tasks.vision.GestureRecognizerOptions
VisionRunningMode = mp.tasks.vision.RunningMode

class HandGestures:
    def __init__(self, model_path: str = "./models/gesture_recognizer.task"):
        self.model_path = model_path
        self.classification_model = ClassificationModel()
        self.result: Any = None
        self._recognizer: Any = None

    def _on_result(self, result: Any, output_image: mp.Image, timestamp_ms: int):
        self.result = result

    def start(self):
        options = GestureRecognizerOptions(
            base_options=BaseOptions(model_asset_path=self.model_path),
            running_mode=VisionRunningMode.LIVE_STREAM,
            result_callback=self._on_result,
        )
        self._recognizer = GestureRecognizer.create_from_options(options)

    def process_async(self, frame: cv.Mat, timestamp_ms: int):
        if self._recognizer is None:
            raise RuntimeError("HandGestures.start() must be called before process_async()")
        rgb_frame = cv.cvtColor(frame, cv.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame)
        self._recognizer.recognize_async(mp_image, timestamp_ms)

    def draw(self, frame: cv.Mat):
        if not self.result or not self.result.hand_landmarks:
            return

        for hand_landmarks in self.result.hand_landmarks:
            draw_hand_landmarks(frame, hand_landmarks)

        gesture = self.classification_model.classify_gesture(self.result.hand_landmarks[0])
        cv.putText(frame, gesture, (10, 30), cv.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2, cv.LINE_AA)

    def close(self):
        if self._recognizer is not None:
            self._recognizer.close()
            self._recognizer = None
