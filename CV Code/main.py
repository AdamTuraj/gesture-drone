import time

import cv2 as cv

from src.detect_face import FaceDetection
from src.hand_gestures import HandGestures


def main():
    cap = cv.VideoCapture(0)
    if not cap.isOpened():
        raise RuntimeError("Cannot open camera")

    face_detection = FaceDetection()
    hand_gestures = HandGestures()
    face_detection.start()
    hand_gestures.start()

    start_time = time.monotonic()
    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("Can't receive frame (stream end?). Exiting ...")
                break

            timestamp_ms = int((time.monotonic() - start_time) * 1000)
            face_detection.process_async(frame, timestamp_ms)
            hand_gestures.process_async(frame, timestamp_ms)

            face_detection.draw(frame)
            hand_gestures.draw(frame)

            cv.imshow("Face + Hand Gestures", frame)
            if cv.waitKey(1) & 0xFF == 27:
                break
    finally:
        face_detection.close()
        hand_gestures.close()
        cap.release()
        cv.destroyAllWindows()


if __name__ == "__main__":
    main()
    