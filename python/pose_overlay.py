"""Drawing what the pose system thinks it is seeing.

Used in two places: the startup warmup, where it proves the model works before a
mouse goes in, and any after-the-fact check of a live decision.

The point of drawing it at all is that a 180 degree heading error looks perfectly
reasonable as a number and is instantly obvious as an arrow. The ear-swap flip
correction is the subtlest part of the whole system, so the one thing this is
built to make unmissable is which way the mouse is facing.
"""

from __future__ import annotations

import math
from typing import Optional, Sequence

import numpy as np

# BGR, since OpenCV. Ears are the pair that can be swapped, so they get two
# strongly different colours rather than two shades of one.
PART_COLOURS = {
    "nose": (160, 160, 160),
    "head": (200, 200, 200),
    "left_ear": (80, 80, 255),       # red-ish
    "right_ear": (255, 200, 80),     # blue-ish
    "spine_1": (0, 215, 255),
    "spine_2": (0, 255, 200),
    "spine_3": (255, 255, 0),
    "spine_4": (255, 0, 200),
}

UNUSED_PARTS = ("nose", "head")      # drawn faintly: the heading never reads them

HEADING_COLOUR = (0, 255, 0)
FLIPPED_COLOUR = (0, 180, 255)       # amber when the flip correction fired
PORT_COLOUR = (200, 200, 200)
CUE_COLOUR = (0, 255, 255)


def to_bgr(image: np.ndarray) -> np.ndarray:
    """Mono8 to a 3-channel image that can be drawn on in colour."""
    import cv2
    if image.ndim == 2:
        return cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
    return image.copy()


def draw_pose(
    image: np.ndarray,
    keypoints: dict,
    result=None,
    *,
    port_coordinates: Optional[Sequence[tuple[float, float]]] = None,
    cue_port: Optional[int] = None,
    min_likelihood: float = 0.6,
    scale: float = 1.0,
) -> np.ndarray:
    """Annotate a frame with the keypoints and the heading that came from them.

    ``result`` is a ``head_angle.HeadAngle``; without one only the keypoints are
    drawn. ``scale`` thickens the lines for a montage that will be shrunk.
    """
    import cv2

    canvas = to_bgr(image)
    thickness = max(1, int(round(2 * scale)))
    radius = max(2, int(round(4 * scale)))

    # The spine, as a chain, so a mis-ordered spine is visible as a zigzag.
    spine = [keypoints.get(f"spine_{i}") for i in range(1, 5)]
    for a, b in zip(spine, spine[1:]):
        if a is None or b is None:
            continue
        if a.likelihood < min_likelihood or b.likelihood < min_likelihood:
            continue
        cv2.line(canvas, (int(a.x), int(a.y)), (int(b.x), int(b.y)),
                 (120, 120, 120), thickness, cv2.LINE_AA)

    # The ear line: the vector the heading is perpendicular to.
    left, right = keypoints.get("left_ear"), keypoints.get("right_ear")
    if left is not None and right is not None:
        cv2.line(canvas, (int(left.x), int(left.y)), (int(right.x), int(right.y)),
                 (200, 200, 200), thickness, cv2.LINE_AA)

    for name, point in keypoints.items():
        if point.likelihood < min_likelihood:
            continue
        colour = PART_COLOURS.get(name, (255, 255, 255))
        if name in UNUSED_PARTS:
            colour = tuple(int(c * 0.4) for c in colour)
        cv2.circle(canvas, (int(point.x), int(point.y)), radius, colour, -1, cv2.LINE_AA)

    if result is None:
        return canvas

    # The heading arrow, from the eyes-offset reference point the port angles are
    # actually measured from.
    origin = (int(result.midpoint[0]), int(result.midpoint[1]))
    length = 90 * scale
    bearing_rad = math.radians(result.bearing)
    tip = (int(origin[0] + length * math.cos(bearing_rad)),
           int(origin[1] - length * math.sin(bearing_rad)))

    colour = FLIPPED_COLOUR if result.angle_correction_method == "ear_flip_corrected" \
        else HEADING_COLOUR
    cv2.arrowedLine(canvas, origin, tip, colour,
                    max(2, int(round(3 * scale))), cv2.LINE_AA, tipLength=0.25)
    cv2.circle(canvas, origin, max(2, int(round(3 * scale))), colour, -1, cv2.LINE_AA)

    if port_coordinates:
        for index, (port_x, port_y) in enumerate(port_coordinates):
            is_cue = cue_port is not None and index == int(cue_port) - 1
            port_colour = CUE_COLOUR if is_cue else PORT_COLOUR
            cv2.circle(canvas, (int(port_x), int(port_y)),
                       max(4, int(round(8 * scale))), port_colour,
                       max(1, int(round(2 * scale))), cv2.LINE_AA)
            cv2.putText(canvas, str(index + 1),
                        (int(port_x) + 10, int(port_y) - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5 * scale, port_colour,
                        max(1, int(round(1 * scale))), cv2.LINE_AA)
            if is_cue:
                cv2.line(canvas, origin, (int(port_x), int(port_y)),
                         CUE_COLOUR, max(1, int(round(1 * scale))), cv2.LINE_AA)

    return canvas


def label_panel(image: np.ndarray, lines: Sequence[tuple[str, tuple]],
                *, scale: float = 1.0) -> np.ndarray:
    """A small caption block in the top-left, over a darkened strip.

    Over a strip rather than straight onto the picture, because arena footage is
    bright in the middle and dark at the edges and text on its own is unreadable
    on one or the other.
    """
    import cv2

    canvas = image
    font_scale = 0.55 * scale
    line_height = int(round(24 * scale))
    pad = int(round(8 * scale))
    height = line_height * len(lines) + pad

    strip = canvas[0:height, :].astype(np.float32) * 0.35
    canvas[0:height, :] = strip.astype(np.uint8)

    y = int(round(18 * scale))
    for text, colour in lines:
        cv2.putText(canvas, text, (pad, y), cv2.FONT_HERSHEY_SIMPLEX,
                    font_scale, colour, max(1, int(round(1.4 * scale))), cv2.LINE_AA)
        y += line_height
    return canvas


def montage(images: Sequence[np.ndarray], columns: int = 3,
            cell_width: int = 420) -> np.ndarray:
    """Tile annotated frames into one image, for the warmup window.

    Everything is scaled to a common cell size; frames of different sizes tile
    without complaint, which matters because reference images may come from
    different cameras.
    """
    import cv2

    if not images:
        return np.zeros((100, 100, 3), dtype=np.uint8)

    columns = max(1, min(columns, len(images)))
    rows = (len(images) + columns - 1) // columns

    first_height, first_width = images[0].shape[:2]
    cell_height = int(round(cell_width * first_height / first_width))

    canvas = np.zeros((rows * cell_height, columns * cell_width, 3), dtype=np.uint8)
    for index, image in enumerate(images):
        row, column = divmod(index, columns)
        resized = cv2.resize(image, (cell_width, cell_height),
                             interpolation=cv2.INTER_AREA)
        canvas[row * cell_height:(row + 1) * cell_height,
               column * cell_width:(column + 1) * cell_width] = resized

    # Grid lines, so adjacent dark frames do not read as one image.
    for row in range(1, rows):
        cv2.line(canvas, (0, row * cell_height), (canvas.shape[1], row * cell_height),
                 (60, 60, 60), 1)
    for column in range(1, columns):
        cv2.line(canvas, (column * cell_width, 0), (column * cell_width, canvas.shape[0]),
                 (60, 60, 60), 1)
    return canvas
