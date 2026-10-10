import math
from dataclasses import dataclass


@dataclass(frozen=True)
class Metrics:
    cell_width: int
    cell_height: int
    box_thickness: int

    @property
    def tag(self):
        return f"{self.cell_width}x{self.cell_height}+{self.box_thickness}"

    @classmethod
    def from_face(cls, face, cell_w, cell_h):
        """Lines as thick as the face's underline at the cell's pixel size (face: upem, advance and
        underline_thickness in font units)."""
        ppem = cell_w * face.upem / face.advance
        return cls(cell_w, cell_h, max(1, math.ceil(face.underline_thickness * ppem / face.upem)))
