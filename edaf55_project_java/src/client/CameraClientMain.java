package client;

import networking.*;

public class CameraClientMain {

	//Manage client side classes
	public static void main(String[] args) {
		
		int camNbr1 = 1, camNbr2 = 2;
		//Subject to change
		String ipCam1 = "127.0.0.1";
		String ipCam2 = "127.0.0.1";
		
		SwingGui gui = new SwingGui();
		ClientMonitor cm = new ClientMonitor();
		DisplayMonitor dm = new DisplayMonitor(gui);
		
		Display display1 = new Display(dm, cm, camNbr1);
		Display display2 = new Display(dm, cm, camNbr2);
		Input input = new Input(cm, gui);
		
		PictureNetStarter picCam1 = new PictureNetStarter(cm, ipCam1, 8002, camNbr1);
		PictureNetStarter picCam2 = new PictureNetStarter(cm, ipCam2, 8005, camNbr1);
		ModeNetStarter modeCam1 = new ModeNetStarter(cm, ipCam1, 8001);
		ModeNetStarter modeCam2 = new ModeNetStarter(cm, ipCam2, 8004);
		
		gui.StartGui();
		display1.start();
		display2.start();
		modeCam1.startMe();
		modeCam2.startMe();
	}
	
}