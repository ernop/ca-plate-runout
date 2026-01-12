# License Plate Runout Analysis - Agent Requirements

## Project Purpose
Analyze California license plate issuance patterns to predict when the 9-series (ending at 9ZZZ999) will be exhausted and the new format (000AAA0) will begin.

---

## Visualization Requirements

### 1. Core Purpose: Track End of 9-Series
- **Primary goal**: Predict when 9ZZZ999 will be issued (the FINAL plate before format change)
- **New format**: After 9ZZZ999, California will switch to '000AAA0' format (3 numbers, 3 letters, 1 number)
- **Source**: LA Times reports transition expected in 2026

### 2. Text Readability - CRITICAL
- **NEVER use grey text on dark backgrounds**
- All text must be **pure white (#FFFFFF)** or **bright saturated colors**
- Minimum font size: 10pt for body text, 12pt for labels, 14pt+ for titles
- Use high-contrast color combinations only

### 3. Space Allocation - No Overlapping
- Each chart/section must have **dedicated, non-overlapping space**
- Explanation text boxes must be **separate from chart areas**
- Use grid layout with explicit margins between sections
- Test at final output resolution to verify no overlap

### 4. Residuals Section Requirements
- **Full explanation required**, not truncated
- Must explain:
  - What residuals represent (observed minus predicted)
  - What positive/negative residuals mean
  - The shaded confidence band meaning
  - Why this matters for prediction accuracy
- Allocate a dedicated text box with sufficient height

### 5. Data Gap Annotation (Nov-Dec 2025)
- **Mark the gap visually** on the trajectory chart
- Draw shaded region showing the gap period
- Include explanatory annotation with:
  - Possible causes: Holiday closures, reduced DMV operations
  - Reporting delays from observers
  - Year-end processing backlogs
- Research context: Vehicle registration surges before price increases

### 6. Data Window Chart Explanation
- **Dedicated explanation box** (not overlapping the chart)
- Must explain:
  - What "data windows" mean (5yr, 3yr, 1yr, 6mo, 3mo, 1mo)
  - How different windows affect predictions
  - Why recent data may differ from historical trends
  - Which window is most reliable and why

### 7. Epistolary Nature
- This is an **explanatory/educational image**
- Include more context and reasoning throughout
- Every chart should have accompanying explanation
- Reader should understand methodology without external docs

### 8. Image Generation Technical Requirements
- Output: PNG at fixed resolution (e.g., 2700x2550 pixels at 150 DPI = 18x17 inches)
- Use matplotlib with explicit figure size and DPI
- Verify pixel positions don't cause overlap
- Font sizes must be large enough to read at final size

---

## Color Palette (Dark Theme)
```
Background:    #0d1117 (dark blue-grey)
Primary:       #58a6ff (bright blue)
Secondary:     #f78166 (coral/orange)
Accent:        #7ee787 (bright green)
Warning:       #d29922 (gold)
Text:          #FFFFFF (pure white) - NEVER grey
Grid:          #21262d (subtle dark)
Error/Alert:   #ff6b6b (red)
```

---

## Research Findings

### California License Plate Transition
- **Current series**: 9AAA000 through 9ZZZ999
- **Final plate**: 9ZZZ999 (predicted mid-2026)
- **Next format**: 000AAA0 (three numbers, three letters, one number)
- **Source**: LA Times, April 2025

### Data Gap Causes (Nov-Dec 2025)
- Holiday period reduced DMV operations
- Fewer observers submitting sightings during holidays
- Year-end vehicle registration processing delays
- Possible surge in registrations before anticipated price increases

---

## Self-Testing Loop (Non-Blocking)

**IMPORTANT**: Use the `Agg` backend to prevent blocking popups:
```python
import matplotlib
matplotlib.use('Agg')  # MUST be before importing pyplot
```

This allows continuous iteration without waiting for window closes.

### Iteration Workflow:
1. Run `python calc.py` → generates PNG instantly (no popup)
2. View with `read_file` tool → see image inline
3. Check for:
   - Any grey or low-contrast text
   - Any overlapping elements
   - Truncated explanations
   - Missing annotations
4. Make fixes and repeat until all requirements met

---

## File Structure
- `calc.py` - Main analysis and visualization code
- `agents.md` - This requirements document
- `plate_analysis*.png` - Generated output images

---

## Implementation Notes (Updated 2026-01-12)

### Figure Layout
- Figure size: 24x32 inches at 120 DPI
- 8-row GridSpec layout with explicit height ratios
- Proper spacing: hspace=0.45, wspace=0.35

### Key Sections
1. **Title row** - Main title
2. **Disclaimer row** - Important caveats and next format info
3. **Main trajectory + sidebar** - Scatter plot with milestones, data gap annotation, and estimates by window
4. **Residuals + explanation + remaining** - Model fit analysis with full explanation
5. **Timeline** - Visual series progression with TODAY and FINAL markers
6. **Rate chart + explanation** - Issuance rate comparison with methodology
7. **Methodology summary** - Data source, model, prediction, limitations
8. **Footer** - Generation timestamp and data summary

### Data Gap Annotation
- Shaded region from Oct 29 - Dec 1, 2025
- Annotation box positioned in clear area with arrow to gap
- Explains possible causes: holiday prep, reduced DMV ops, reporting delays

### Text Standards
- All text: Pure white (#FFFFFF) or bright saturated colors
- Never use grey text on dark backgrounds
- Monospace font for explanation boxes
- Sans-serif for titles and labels
