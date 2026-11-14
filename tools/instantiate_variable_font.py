import argparse

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input_font")
    parser.add_argument("output_font")
    parser.add_argument("--weight", type=float, default=600)
    args = parser.parse_args()

    font = TTFont(args.input_font)
    axes = {axis.axisTag: axis for axis in font["fvar"].axes}
    if "wght" not in axes:
        raise SystemExit("The source font has no variable wght axis")
    axis = axes["wght"]
    if not axis.minValue <= args.weight <= axis.maxValue:
        raise SystemExit(
            f"Weight {args.weight} is outside the font range "
            f"{axis.minValue}..{axis.maxValue}"
        )

    static_font = instantiateVariableFont(font, {"wght": args.weight}, inplace=True)
    static_font.save(args.output_font)
    print(f"Created static font at wght={args.weight:g}: {args.output_font}")


if __name__ == "__main__":
    main()
