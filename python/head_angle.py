"""Head bearing from pose keypoints, as the offline analysis computes it.

A faithful port of ``Session_nwb.find_angles`` from hex_behav_analysis. The live
system and the offline pipeline must agree about what a mouse's heading was on a
given frame, or nothing built on live tracking can be checked afterwards. So this
reproduces that function's behaviour including its quirks, and
``tests/test_head_angle.py`` compares the two on the same inputs.

The algorithm, in outline:

1. The heading is perpendicular to the vector between the ears.
2. DeepLabCut sometimes swaps the ears, which puts that heading 180 degrees out.
   The spine catches it: if the spine disagrees by more than 90 degrees *and* a
   majority of spine points sit on the wrong side of the ear line, flip.
3. When the ears are too close together to give a reliable perpendicular -- which
   happens whenever the mouse looks up or down relative to the camera -- fall
   back to a line fitted through the spine.
4. The reference point is the ear midpoint pushed 40 px forward along the
   heading, approximating where the eyes are.
5. Port angles are measured from that point, so they depend on where the mouse
   actually is rather than assuming it is centred.

Two constants are in pixels and therefore tied to the frame size they were tuned
at: ``MINIMUM_EAR_DISTANCE`` and ``EYES_OFFSET``. The offline analysis declares
1280x1080 and the current camera is 1280x1024 -- same width, so the horizontal
scale matches and both carry over. **All coordinates here must be in full-frame
pixel space.** If a caller crops before inference, it must map coordinates back
before calling this, or these two constants and the port coordinates all quietly
mean the wrong thing.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Optional, Sequence

import numpy as np

# Below this ear separation the perpendicular is unreliable; use the spine.
MINIMUM_EAR_DISTANCE = 50.0

# How far forward of the ear midpoint the reference point sits, approximating
# the eyes rather than the centre of the skull.
EYES_OFFSET = 40.0

# The keypoints actually used. Nose and head are ignored: the offline analysis
# never reads them. They cost nothing to leave in the model, because a DLC model
# emits every keypoint it was trained on whether anyone reads it or not.
EAR_PARTS = ("left_ear", "right_ear")
SPINE_PARTS = ("spine_1", "spine_2", "spine_3", "spine_4")
REQUIRED_PARTS = EAR_PARTS + SPINE_PARTS


@dataclass
class Keypoint:
    x: float
    y: float
    likelihood: float = 1.0

    @property
    def finite(self) -> bool:
        return bool(np.isfinite(self.x) and np.isfinite(self.y))


@dataclass
class HeadAngle:
    """What the calculation concluded, plus how it got there.

    ``angle_correction_method`` and the likelihoods are the important part for
    anything after the fact: they are how you tell whether a live decision was
    made on good data or on a guess.
    """
    bearing: float                       # degrees, 0-360, 0 = +x, counter-clockwise
    midpoint: tuple[float, float]        # the eyes-offset reference point
    ear_midpoint: tuple[float, float]    # the raw ear midpoint, before the offset
    angle_correction_method: str         # none | ear_flip_corrected | spine_based |
                                         # ears_too_close_no_spine
    ear_distance: float
    spine_data_available: bool
    left_ear_likelihood: float
    right_ear_likelihood: float
    min_likelihood: float                # lowest over every keypoint actually used

    # Filled in when port coordinates are supplied.
    relative_angles: list[float] = field(default_factory=list)
    cue_presentation_angle: Optional[float] = None


def _wrap_180(angle: float) -> float:
    """Fold a 0-360 angle into (-180, 180], as the offline analysis does."""
    if angle > 180:
        return angle - 360
    if angle <= -180:
        return angle + 360
    return angle


def _spine_angle_degrees(spine: Sequence[tuple[float, float]]) -> float:
    """Heading implied by the spine, nose-end first. spine_1 is the head end."""
    vector_x = spine[0][0] - spine[-1][0]
    vector_y = spine[0][1] - spine[-1][1]
    return math.degrees(math.atan2(-vector_y, vector_x)) % 360


def _bearing_from_spine(spine: Sequence[tuple[float, float]]) -> float:
    """Least-squares line through the spine points, direction from spine_1.

    Reproduces the offline version including its guards: a vertical-line special
    case, a numerical-range check, and an atan2 fallback whenever polyfit cannot
    be trusted.
    """
    spine_x = [p[0] for p in spine]
    spine_y = [p[1] for p in spine]

    if len(set(spine_x)) < 2:
        # Every x identical, so the line is vertical and polyfit would blow up.
        # (The offline code has an else branch here that cannot be reached, since
        # identical x values make the difference below zero. Not reproduced.)
        if spine[0][1] < spine[-1][1]:
            theta_rad = math.pi / 2          # head above tail in image coords
        else:
            theta_rad = -math.pi / 2
        return math.degrees(theta_rad) % 360

    valid = [(x, y) for x, y in zip(spine_x, spine_y)
             if np.isfinite(x) and np.isfinite(y)]
    if len(valid) >= 2:
        xs = [p[0] for p in valid]
        ys = [p[1] for p in valid]
        x_range = max(xs) - min(xs)
        if x_range > 1e-6 and not any(abs(x) > 1e6 for x in xs):
            try:
                slope = float(np.polyfit(xs, ys, 1)[0])
                if spine[0][0] > spine[-1][0]:
                    theta_rad = math.atan(-slope)       # head right of tail
                else:
                    theta_rad = math.atan(-slope) + math.pi
                return math.degrees(theta_rad) % 360
            except Exception:
                pass

    return _spine_angle_degrees(spine)


def head_angle(
    keypoints: dict[str, Keypoint],
    *,
    port_coordinates: Optional[Sequence[tuple[float, float]]] = None,
    correct_port: Optional[int] = None,
    minimum_ear_distance: float = MINIMUM_EAR_DISTANCE,
    eyes_offset: float = EYES_OFFSET,
) -> Optional[HeadAngle]:
    """Bearing and port angles from one frame of keypoints, in full-frame pixels.

    ``keypoints`` maps bodypart name to Keypoint. Only the ears and spine_1..4 are
    read. Returns None when neither ear has finite coordinates, which is the one
    case the offline version also refuses.

    ``correct_port`` is 1-based, matching the LED/sensor numbering, and indexes
    ``port_coordinates`` as ``correct_port - 1``.
    """
    left = keypoints.get("left_ear")
    right = keypoints.get("right_ear")
    if left is None or right is None or not left.finite or not right.finite:
        return None

    average_left_ear = (left.x, left.y)
    average_right_ear = (right.x, right.y)

    ear_vector_x = average_right_ear[0] - average_left_ear[0]
    ear_vector_y = average_right_ear[1] - average_left_ear[1]
    ear_distance = math.hypot(ear_vector_x, ear_vector_y)

    angle_correction_method = "none"

    # The offline version requires every one of spine_1..spine_4 to be present and
    # finite; a single missing point makes the whole spine unavailable.
    spine_positions: list[tuple[float, float]] = []
    spine_available = True
    for name in SPINE_PARTS:
        point = keypoints.get(name)
        if point is None or not point.finite:
            spine_available = False
            break
        spine_positions.append((point.x, point.y))

    if ear_distance >= minimum_ear_distance:
        # The heading is perpendicular to the ear vector. Rotating the y-flipped
        # ear vector by 90 degrees gives exactly this, which is why the older
        # PP_cue_offset_heading.py form -- atan2(-vy, vx) then +90 -- is the same
        # calculation and not a discrepancy to resolve.
        head_vector_x = ear_vector_y
        head_vector_y = -ear_vector_x
        bearing = math.degrees(math.atan2(-head_vector_y, head_vector_x)) % 360

        if spine_available and len(spine_positions) >= 2:
            spine_angle = _spine_angle_degrees(spine_positions)
            angle_diff = abs(bearing - spine_angle)
            if angle_diff > 180:
                angle_diff = 360 - angle_diff

            # Only even consider a flip when the spine disagrees badly, and then
            # only on a majority vote, so one bad spine point cannot flip a good
            # heading.
            if angle_diff > 90:
                flip_votes = 0
                for spine_x, spine_y in spine_positions:
                    to_spine_x = spine_x - average_left_ear[0]
                    to_spine_y = spine_y - average_left_ear[1]
                    cross = ear_vector_x * to_spine_y - ear_vector_y * to_spine_x
                    if cross < 0:
                        flip_votes += 1
                if flip_votes > len(spine_positions) / 2:
                    bearing = (bearing + 180) % 360
                    angle_correction_method = "ear_flip_corrected"

    elif spine_available and len(spine_positions) >= 2:
        angle_correction_method = "spine_based"
        bearing = _bearing_from_spine(spine_positions)

    else:
        # Ears too close and no spine to fall back on. The perpendicular is used
        # anyway, because some answer beats none, but the method records that it
        # should not be trusted.
        angle_correction_method = "ears_too_close_no_spine"
        head_vector_x = ear_vector_y
        head_vector_y = -ear_vector_x
        bearing = math.degrees(math.atan2(-head_vector_y, head_vector_x)) % 360

    ear_midpoint = (
        (average_left_ear[0] + average_right_ear[0]) / 2,
        (average_left_ear[1] + average_right_ear[1]) / 2,
    )

    # When the spine provided the heading, spine_1 is the reference point rather
    # than the ear midpoint.
    if angle_correction_method == "spine_based" and spine_available and spine_positions:
        base_x, base_y = spine_positions[0]
    else:
        base_x, base_y = ear_midpoint

    bearing_rad = math.radians(bearing)
    midpoint = (
        base_x + eyes_offset * math.cos(bearing_rad),
        base_y - eyes_offset * math.sin(bearing_rad),   # y is down in image space
    )

    used = [keypoints[name] for name in REQUIRED_PARTS if name in keypoints]
    min_likelihood = min((k.likelihood for k in used), default=0.0)

    result = HeadAngle(
        bearing=bearing,
        midpoint=midpoint,
        ear_midpoint=ear_midpoint,
        angle_correction_method=angle_correction_method,
        ear_distance=ear_distance,
        spine_data_available=spine_available,
        left_ear_likelihood=left.likelihood,
        right_ear_likelihood=right.likelihood,
        min_likelihood=min_likelihood,
    )

    if port_coordinates:
        result.relative_angles = port_angles(bearing, midpoint, port_coordinates)
        if correct_port is not None:
            index = int(correct_port) - 1
            if 0 <= index < len(result.relative_angles):
                result.cue_presentation_angle = _wrap_180(
                    result.relative_angles[index] % 360)

    return result


def port_angles(
    bearing: float,
    midpoint: tuple[float, float],
    port_coordinates: Sequence[tuple[float, float]],
) -> list[float]:
    """Each port's angle relative to the mouse's heading, 0-360.

    Measured from where the mouse actually is, not from the centre of the arena.
    Index 0 is LED_1/SENSOR1 through index 5 for LED_6/SENSOR6 -- the same
    convention as the offline port tables, and getting it off by one rotates
    every angle by about 60 degrees. That has happened once already.
    """
    bearing_rad = math.radians(bearing)
    out = []
    for port_x, port_y in port_coordinates:
        vector_x = port_x - midpoint[0]
        vector_y = port_y - midpoint[1]
        port_angle_rad = math.atan2(-vector_y, vector_x)
        out.append(math.degrees(port_angle_rad - bearing_rad) % 360)
    return out


def angle_to_port(result: HeadAngle, port: int) -> Optional[float]:
    """Signed angle from the mouse's heading to a 1-based port, in (-180, 180].

    Negative is one way round, positive the other; what a protocol usually wants
    is ``abs(...)`` against a tolerance.
    """
    index = int(port) - 1
    if not result.relative_angles or not (0 <= index < len(result.relative_angles)):
        return None
    return _wrap_180(result.relative_angles[index] % 360)
