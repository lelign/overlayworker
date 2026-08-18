It's for Intel Arria 10 custom board
Draw ARGB 1920*1080 QT-image, convert it to YUV 4:2:2 (Cb/Y/Alpha/Cr/Y/Alpha)
and send it via DMA to FPGA-part of Arria 10

The algorithm looks like this:

1. In main, parse /home/root/control.txt

2. Fill sceneData (QList sceneData) with the calculated geometry: SceneElementData calculateCellGeometry(int col, int row, int total_cols, int total_rows, int typeId) 

3. Send sceneData to the worker

4. The worker creates a cashimage for static elements, renders dynamic elements each iteration, and assembles the shared canvas

5. Send the shared canvas to the ARGB > CbYAlpha/CrYAlpha converter

6. Once the shared canvas is written to RAM buffer, switch the buffer. 


Shared canvas be assembled directly in RAM buffer. And the worker will only rewrite dynamic objects.

Moreover, some dynamic objects adjusted. Why rewrite the hour, minute, and second values ​​for a digital clock every frame? This can only be done once per second, while the millisecond values ​​should be rewritten as frequently as possible.
