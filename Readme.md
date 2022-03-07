##Application Instructions
1. Plug the power cable in the power port, the PS/2 keyboard in the PS/2 port, the VGA cable in the VGA port for the Altera DE2-115 board. 
2. Connect the USB-Blaster cable to the USB-Blaster port.
3. Open Quartus Programmer and select USB-Blaster as the currently selected hardware in the Hardware setup window
4. Click on Add File button and select the given "freq_relay_controller.sof" file, and click Start.
5. Open Nios II Software Build Tools for Eclipse and select "../freq_reay_27" as the workspace.
6. If the project does not appear on the workspace, select file> Import > General > Existing Projects into Workspace and select "../freq_relay_27" as the root directory and click Finish.
7. Build the project and right-click the frequencyrelay folder, and select Run As > Nios II Hardware. If "Unable to validate connection settings" error shows up, go to Run > Run configurations and select the Target Connection tab. Click on Refresh Connections and Run.

##System Controls:
-	Use the Push button "Key1" to request Maintenance mode.
-	Use the switches SW0 to SW4 to turn loads on or off.
-	Use the keyboard's number keys to enter the value for the thresholds or Backspace to delete a character. (Only in maintenance mode)
-	Press "F" to set the entered value as the frequency threshold or "R" to set as the rate of change threshold.
-	Only ten characters are allowed as the inputs (including the decimal).

