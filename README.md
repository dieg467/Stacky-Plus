# Stacky-Plus, 4 menu modes for the Windows taskbar  

### Original Source <br>
This project is based on (1) Pawel Turlejski's original project (https://github.com/pawelt/stacky), which is no longer maintained.
(2) Clau-Bucur's fork (https://github.com/clau-bucur/stacky)

## Description 
This fork expands Stacky with menu modes. All the changes in this fork were made using Artificial Intelligence and Visual Studio.
There are 4 possible menu modes, all from the same .exe file:
- MENU WITH SUBMENUS (default)
- ICON GRID
- ICON GRID WITH NAMES
- DOUBLE COLUMN OF ICONS WITH ICON SUBMENUS

<img width="572" height="486" alt="00a- EscritorioW-Submenú" src="https://github.com/user-attachments/assets/731258f9-303e-44b7-8184-524778f3fd5f" />
<img width="572" height="264" alt="00b- EscritorioW-Cuadrícula" src="https://github.com/user-attachments/assets/f45b75f5-6405-41d4-bdfc-950579f09dd7" />
<img width="572" height="239" alt="00c- EscritorioW-Cuadrícula-name" src="https://github.com/user-attachments/assets/8fb9a338-2c9d-45e5-947f-2792c8b279fb" />
<img width="572" height="341" alt="00d- EscritorioW-2C" src="https://github.com/user-attachments/assets/4b1b0ff6-b1be-4112-8667-6631010b958d" />

## **How many menus can I create on the taskbar?**
As many as you want, and in any of the modes. Each one will have its own dedicated button on the taskbar.

## Download
The .exe file can be downloaded from https://github.com/dieg467/Stacky-Plus/releases/tag/Stacky-Plus

## STEPS TO FOLLOW
This application can be downloaded as an .exe file, but it does not need to be installed. There's no need to double-click the .exe file.
The following steps explain how to create a menu on the taskbar.

**1) CREATING THE CONTAINER FOLDER**. Create a folder anywhere on your computer (on drive C, in My Documents, etc.), and name it STACKY PLUS, for example. Place the Stacky.exe file and all the folders that will be created below inside it.

**2) CREATING FOLDERS** <br>
**a)** Inside the STACKY PLUS folder, create a folder to hold shortcuts. For example, you can create a "Multimedia" folder if you're going to place shortcuts related to music, videos, etc., there.

<img width="655" height="333" alt="02- Carpeta Stacky Plus + Multimedia" src="https://github.com/user-attachments/assets/ce7379ec-456e-43aa-92df-63b8824b3416" />

**b)** In the created folder, place shortcuts to applications, folders, files, websites, etc.

<img width="609" height="333" alt="03-Multimedia" src="https://github.com/user-attachments/assets/cb5a86b7-d523-4f75-92d3-9666612bb41a" />

This folder will later become a menu in the taskbar. If you create several folders, such as "Multimedia", "Documents", "System", etc., each one will become a separate menu in the taskbar, each with its own button. <br>
*Note:* Each folder that will become a menu item does not need to be inside the STACKY PLUS container folder. It can be anywhere on your computer. It can even be a pre-existing folder that already contained shortcuts.

**3) CREATING SUBFOLDERS** <br>
If you want a menu to have submenus, you must create subfolders, as detailed below. <br>
**a)** Inside the folder, create subfolders with the following extension:<br>
`.submenu` <br>
For example, for the "Multimedia" folder you can create the subfolders Music.submenu, Video.submenu, etc.

<img width="655" height="392" alt="04-Submenú Multimedia" src="https://github.com/user-attachments/assets/35738909-c330-435c-a629-0cbf477bb6dd" />

**b)** Place shortcuts to files, folders, applications, web pages, etc., in each subfolder. Each subfolder will become a submenu.<br>
Shortcuts located within a folder but outside of subfolders will be displayed in the main menu. <br>
You can create multiple levels of submenus by creating subfolders (with the .submenu extension) within subfolders.<br>
*Note:* The "Icon Grid" and "Named Icon Grid" modes do not support submenus.

**4) CREATING SEPARATORS** <br>
In the folder or subfolder where you want to create a separator (horizontal dividing line), create a .txt text file, give it a name, and change the .txt extension to the following: <br>
`.separator`<br>
For example, it could be named aa.separator, Multimedia.separator, zz.separator, etc.

<img width="655" height="423" alt="05- Separador" src="https://github.com/user-attachments/assets/e974381a-d901-47fa-8395-d6e05fbf8454" />

You can create multiple separators within the same menu or submenu. The position of a separator depends on its name, as they are ordered alphabetically.<br>
If you don't want separators in the menu, you can skip this step. <br>
*Note:* Separators are only visible in "Menu with Submenus" mode.

**5) ORDERING ITEMS** <br>
By default, items (folder, subfolder, shortcut, separator) are sorted alphabetically, and will be sorted accordingly in the corresponding menu or submenu. <br>
However, if you want to customize the order of the items, the first item must begin with the prefix %01%, followed by its name. There is no need to leave a space between %01% and the item name. The %01% prefix will not be displayed in the menu. <br>
Subsequent items will be named %02%, %03%, etc. If an item does not have a prefix, it will be sorted alphabetically.

<img width="752" height="423" alt="06a- Ordenamiento" src="https://github.com/user-attachments/assets/02dfcbc5-fb3a-43a5-b2b1-dea9af488771" />

**6) CREATING THE MENU** <br>
**a)** Right-click on the created folder, such as "Multimedia", and click "Copy as path." This copies the folder path.

<img width="655" height="372" alt="07a Copiar ruta carpeta" src="https://github.com/user-attachments/assets/eb86381f-e013-4aac-a9f3-c6b535cc6e16" />

**b)** Right-click on the Stacky.exe file and select "Create shortcut."

<img width="655" height="338" alt="08a Copiar acceso directo Stacky" src="https://github.com/user-attachments/assets/ba07de49-ff20-4b23-92a3-1a426ce52528" />

**c)** Rename the shortcut, giving it a name that associates it with the corresponding folder. For example, for the "Multimedia" folder, you could name it "MMed" or "M", etc.

**d)** Right-click on the shortcut and go to Properties.

<img width="677" height="375" alt="11a Propiedades" src="https://github.com/user-attachments/assets/33a4e5af-b669-4363-b134-2a0ffe51f687" />

**e)** In the Target field, without deleting anything, go to the end of the text, leave a space, and paste (by pressing Ctrl+V on the keyboard) the folder path that was copied in step a). It should look something like this:

`"C:\STACKY PLUS\stacky.exe" "C:\STACKY PLUS\Multimedia"`

<img width="363" height="509" alt="12- Casilla Destino" src="https://github.com/user-attachments/assets/6308ad0c-1688-4058-9794-3d579680d652" />

**f)** You can customize the shortcut icon by clicking "Change Icon" within Properties. This shortcut's icon will be the one that appears as the menu button on the Taskbar. <br>
*Note:* If you now double-click the shortcut, the created menu will open without icons. Double-clicking it again will open it with the icons. This allows you to verify that everything is as desired before creating the button on the taskbar.

**g)** Drag the shortcut to the taskbar and drop it in the desired position. This will create the menu button.

<img width="201" height="423" alt="14a- Botón de menú" src="https://github.com/user-attachments/assets/2d4a65b9-3f7d-4843-9473-a40a0039172b" />

**h)** Clicking the button created on the taskbar will open the menu. <br>
The first time you open the menu, the icons may not be visible. In that case, click the button again to open the menu. This time, the icons should be visible. <br>
*Note:* This only happens the first time the menu is opened, once it has been created, or when any modification is made to it.

## Shortcuts to Web Pages
By default, website shortcuts display the browser icon (Chrome, Edge, Firefox, etc.). The icon shown in the menu can be customized in two ways:
- Automatically with the website's favicon. To do this, access the website by clicking the shortcut in the created menu. Most icons are supported.
- With an icon stored on your computer, by accessing the properties of the corresponding shortcut.

## MENU MODIFICATIONS
- **MODIFYING ELEMENTS**. Changes can be made to folders and subfolders, even after the menu button has been created. You can add or remove shortcuts from the menu or submenu, add or remove submenu folders, separators, etc.
After any changes, clicking the button in the taskbar will open the menu without icons. Clicking the button again will display the corresponding icons.
- **MODIFY MENU MODE**. If you change the menu mode, for example, from "Menu and Submenu" to "Grid" (or another mode), you must delete the existing button from the taskbar and create a new button by dragging the modified shortcut. This will update the changes.

## GRID MENU
To display the menu as a grid of icons, right-click the corresponding shortcut, go to Properties, and at the end of the Target field (without deleting anything), leave a space and add: <br>
```--Options iconmenu-NN``` <br>
NN should be replaced with a number indicating the number of columns to display. For example, to display a 3-column menu, type <br>
```--Options iconmenu-03```

<img width="572" height="264" alt="00b- EscritorioW-Cuadrícula" src="https://github.com/user-attachments/assets/ab4a52c5-463d-4dc7-bf47-d6e84af2e99a" />

## GRID MENU WITH NAMES
To display the created menu as a grid of icons with its name at the bottom, right-click on the corresponding shortcut, go to Properties, and at the end of the Target field (without deleting anything), leave a blank space and add: <br>
```--Options iconmenu-NN-name``` <br>
NN should be replaced with a number indicating the number of columns to display. For example, to display a 3-column menu, type <br>
```--Options iconmenu-03-name```

<img width="572" height="239" alt="00c- EscritorioW-Cuadrícula-name" src="https://github.com/user-attachments/assets/5c231bab-3ca8-4bf5-8ffb-2be4ec4f4798" />

## DOUBLE-COLUMN ICON MENU WITH SUBMENU <br>
To display the created menu as a grid of two columns of icons with submenus, right-click on the corresponding shortcut, go to Properties, and at the end of the Target field (without deleting anything), leave a blank space and add: <br>
```--Options iconmenu-C2``` <br>
If any icon in the main menu corresponds to a submenu, it will open to the right or left, depending on its location. If there are multiple levels of submenus, they will all open to the right or left, as appropriate.

<img width="572" height="341" alt="00d- EscritorioW-2C" src="https://github.com/user-attachments/assets/38997391-c825-4fb7-afa2-1bac2fbb8044" />

## DARK MODE <br>
To display a menu in dark mode, right-click on the corresponding shortcut, go to Properties, and at the end of the Target field (without deleting anything), leave a blank space and add: <br>
```--dark-mode```

*Note:* All menu types support dark mode. At the end of the Target field, you should see: <br>
```--dark-mode```<br>
for the default MENU WITH SUBMENU mode

```--Options iconmenu-NN --dark-mode```<br>
for GRID mode

```--Options iconmenu-NN-name --dark-mode``` <br>
for NAMED GRID mode

```--Options iconmenu-C2 --dark-mode``` <br>
for DOUBLE COLUMN OF ICONS WITH SUBMENU mode

<img width="713" height="395" alt="16a- EscritorioW-Modo oscuro" src="https://github.com/user-attachments/assets/bcd29b63-0dbe-43f7-bc5b-761d377efbe5" />
