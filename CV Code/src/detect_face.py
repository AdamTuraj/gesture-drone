from typing import Any

import cv2 as cv
import mediapipe as mp

BaseOptions = mp.tasks.BaseOptions
FaceDetector = mp.tasks.vision.FaceDetector
FaceDetectorOptions = mp.tasks.vision.FaceDetectorOptions
VisionRunningMode = mp.tasks.vision.RunningMode

class FaceDetection:
    def __init__(self,  model_path: str = "./models/blaze_face_full_range.tflite"):
        self.model_path = model_path
        self.result: Any = None
        self._detector: Any = None

    def _bbox_to_pixels(self, bbox: Any, frame_width: int, frame_height: int) -> tuple[int, int, int, int]:
        values = [bbox.origin_x, bbox.origin_y, bbox.width, bbox.height]
        is_normalized = all(0.0 <= value <= 1.0 for value in values)

        if is_normalized:
            x_min = int(bbox.origin_x * frame_width)
            y_min = int(bbox.origin_y * frame_height)
            width = int(bbox.width * frame_width)
            height = int(bbox.height * frame_height)
        else:
            x_min = int(bbox.origin_x)
            y_min = int(bbox.origin_y)
            width = int(bbox.width)
            height = int(bbox.height)

        x_min = max(0, min(x_min, frame_width - 1))
        y_min = max(0, min(y_min, frame_height - 1))
        x_max = max(0, min(x_min + width, frame_width - 1))
        y_max = max(0, min(y_min + height, frame_height - 1))

        return x_min, y_min, x_max, y_max
        
    def _on_result(self, result: Any, output_image: mp.Image, timestamp_ms: int):
        self.result = result

    def start(self):
        options = FaceDetectorOptions(
            base_options=BaseOptions(model_asset_path=self.model_path),
            running_mode=VisionRunningMode.LIVE_STREAM,
            result_callback=self._on_result,
        )
        self._detector = FaceDetector.create_from_options(options)

    def process_async(self, frame: cv.Mat, timestamp_ms: int):
        if self._detector is None:
            raise RuntimeError("FaceDetection.start() must be called before process_async()")
        rgb_frame = cv.cvtColor(frame, cv.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame)
        self._detector.detect_async(mp_image, timestamp_ms)

    def draw(self, frame: cv.Mat):
        if not self.result or not self.result.detections:
            return
        for face in self.result.detections:
            bbox = face.bounding_box
            x_min, y_min, x_max, y_max = self._bbox_to_pixels(bbox, frame.shape[1], frame.shape[0])
            if x_max > x_min and y_max > y_min:
                cv.rectangle(frame, (x_min, y_min), (x_max, y_max), (255, 0, 0), 2)

    def close(self):
        if self._detector is not None:
            self._detector.close()
            self._detector = None

