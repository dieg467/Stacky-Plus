# Stacky-Plus, 4 menu modes for the Windows taskbar  

### Original Source <br>
This project is based on Pawel Turlejski's original project (https://github.com/pawelt/stacky), which is no longer maintained, and
Clau-Bucur's fork (https://github.com/clau-bucur/stacky)

## Description 
There are 4 possible menu modes, all accessible from the same .exe file:
- MENU WITH SUBMENUS (default)
- ICON GRID (no submenu), WITH AND WITHOUT NAMES
- DOUBLE COLUMN OF ICONS WITH ICON SUBMENUS, WITH AND WITHOUT NAMES
- DOUBLE ROW OF ICONS WITH ICON SUBMENUS, WITH AND WITHOUT NAMES

<img width="572" height="486" alt="00a- EscritorioW-Submenú" src="https://github.com/user-attachments/assets/731258f9-303e-44b7-8184-524778f3fd5f" />

<img width="572" height="239" alt="z11c- EscritorioW-Cuadríc-Cuadríc-name" src="https://github.com/user-attachments/assets/b947bf9f-a4ec-4bb1-a8b3-d4e6b5ae08cc" />

<img width="572" height="384" alt="z13- EscritorioW-2C·y·2C·name" src="https://github.com/user-attachments/assets/8b0ac811-385d-4087-97b1-3d07e65d4bf1" />

<img width="572" height="332" alt="z14- EscritorioW-2F·y·2F-name" src="https://github.com/user-attachments/assets/385fe1fc-bf88-479d-acc5-f9bf82442845" />


## **How many menus can I create on the taskbar?**
As many as you want, and in any of the modes. Each one will have its own dedicated button on the taskbar.

## Download
The stacky-plus.exe file is 348 kB in size and can be downloaded from https://github.com/dieg467/Stacky-Plus/releases/tag/v-2.0
This application downloads as an .exe file, but it does not require installation. You do not need to double-click the .exe file.

## Why is it so fast?
Stacky-Plus stores the icons for all shortcuts in a small file within the corresponding folder. When the user opens a menu, Stacky-Plus accesses its cache immediately, without delay, at full speed.

## STEPS TO FOLLOW
The following steps explain how to create a menu on the taskbar.

**1) CREATING THE CONTAINER FOLDER**. Create a folder anywhere on your computer (on drive C, in My Documents, etc.), and name it STACKY PLUS, for example. Place the stacky.exe file there.

**2) CREATING FOLDERS** <br>
**a)** Inside the STACKY PLUS folder, create a folder to hold shortcuts. For example, you can create a "Multimedia" folder if you're going to place shortcuts related to music, videos, etc., there.

<img width="655" height="333" alt="03-Crear carpeta Multimedia" src="https://github.com/user-attachments/assets/c797668d-0949-4c08-8649-f516b77cf8cf" />

**b)** In the created folder, place shortcuts to applications, folders, files, websites, etc.

<img width="609" height="333" alt="03-Multimedia" src="https://github.com/user-attachments/assets/cb5a86b7-d523-4f75-92d3-9666612bb41a" />

This folder will later become a menu in the taskbar. If you create several folders, such as "Multimedia", "Documents", "System", etc., each one will become a separate menu in the taskbar, each with its own button. <br>
*Note:* Each folder that will become a menu item does not need to be inside the STACKY PLUS container folder. It can be anywhere on your computer. It can even be a pre-existing folder that already contained shortcuts.

**3) CREATING SUBFOLDERS** <br>
If you want a menu to have submenus, you must create subfolders, as detailed below. <br>
**a)** Inside the folder, create subfolders with the following termination:

`.submenu`

The following image shows two submenu subfolders.

<img width="655" height="392" alt="04-Submenú Multimedia" src="https://github.com/user-attachments/assets/35738909-c330-435c-a629-0cbf477bb6dd" />

**b)** Place shortcuts to files, folders, applications, web pages, etc., in each subfolder. Each subfolder will become a submenu.<br>
You can create multiple levels of submenus by creating subfolders (with the .submenu termination) within subfolders.<br>

**4) CREATING SEPARATORS** <br>
In the folder or subfolder where you want to create a separator (horizontal dividing line), create a .txt text file, give it a name, and change the .txt extension to the following:

`.separator`

For example, it could be named aa.separator, Multimedia.separator, zz.separator, etc.

<img width="655" height="423" alt="05- Separador" src="https://github.com/user-attachments/assets/e974381a-d901-47fa-8395-d6e05fbf8454" />

You can create multiple separators within the same menu or submenu. The position of a separator depends on its name, as they are ordered alphabetically.<br>

**5) ORDERING ITEMS** <br>
- **Predetermined**<br>
By default, items (folder, subfolder, shortcut, separator) are sorted alphabetically, and will be sorted accordingly in the corresponding menu or submenu. <br>
- **PERSONALIZED** <br>
If you want to customize the order of the items, the first item must begin with the prefix %01%, followed by its name. There is no need to leave a space between %01% and the item name. The %01% prefix will not be displayed in the menu. <br>
Subsequent items will be named %02%, %03%, etc. If an item does not have a prefix, it will be sorted alphabetically.

<img width="752" height="423" alt="06a- Ordenamiento" src="https://github.com/user-attachments/assets/02dfcbc5-fb3a-43a5-b2b1-dea9af488771" />

**6) CREATING THE MENU** <br>
**a)** Right-click on the created folder, such as "Multimedia", and click "Copy as path." This copies the folder path.

<img width="655" height="372" alt="07-Copiar ruta" src="https://github.com/user-attachments/assets/2849170e-ec12-43dc-9fd2-c8789541a550" />

**b)** Right-click on the Stacky-plus.exe file and select "Create shortcut."

<img width="655" height="338" alt="08a Copiar acceso directo Stacky" src="https://github.com/user-attachments/assets/ba07de49-ff20-4b23-92a3-1a426ce52528" />

**c)** Rename the shortcut, giving it a name that associates it with the corresponding folder. For example, for the "Multimedia" folder, you could name it "MMed" or "M", etc.

**d)** Right-click on the shortcut and go to Properties.

<img width="677" height="375" alt="08-Propiedades acceso directo" src="https://github.com/user-attachments/assets/eb205769-3c75-4699-8a9c-bd3957cf6fef" />

**e)** In the Target field, without deleting anything, go to the end of the text, leave a space, and paste (by pressing Ctrl+V on the keyboard) the folder path that was copied in step a). It should look something like this:

`"C:\STACKY PLUS\stacky-plus.exe" "C:\STACKY PLUS\Multimedia"`

<img width="363" height="509" alt="09-Pegar ruta en casilla Destino" src="https://github.com/user-attachments/assets/4f0976e2-d749-45a4-8a49-5aa94c8df840" />

If you have already completed this step, you can double-click the shortcut to open the created menu. The menu will open without icons. You must double-click it again to open the menu with icons. This allows you to verify that everything is correct before creating the button on the taskbar.

**f)** You can customize the shortcut icon by clicking "Change Icon" within Properties. This shortcut's icon will be the one that appears as the menu button on the Taskbar. <br>

**g)** Drag the shortcut to the taskbar and drop it in the desired position. This will create the menu button.

<img width="201" height="423" alt="14a- Botón de menú" src="https://github.com/user-attachments/assets/2d4a65b9-3f7d-4843-9473-a40a0039172b" />

**h)** Clicking the button created on the taskbar will open the menu. <br>
The first time you open the menu, the icons may not be visible. In that case, click the button again to open the menu. This time, the icons should be visible. <br>
This only happens the first time the menu is opened, once it has been created, or when any modification is made to it.

## Shortcuts to Web Pages
By default, website shortcuts display the browser icon (Chrome, Edge, Firefox, etc.). The icon shown in the menu can be customized in two ways:
- Automatically with the website's favicon. To do this, access the website by clicking the shortcut in the created menu. Most icons are supported.
- With an icon stored on your computer, by accessing the properties of the corresponding shortcut.

## Shortcuts to system files and hidden files
System folders, such as Windows, Program Files, Program Files (x86), etc., are protected and have restricted access. The same applies to hidden folders, such as ProgramData, which contains shortcuts to all the applications visible in the Windows Start Menu. <br>
If you want to create a menu made up of shortcuts to items located in system or hidden folders, it's recommended to create a folder inside the STACKY PLUS container folder. You could name it "SYSTEM," "APPLICATIONS," etc. Then, create (or copy) the shortcuts into this folder. This way, stacky.exe will not be restricted from displaying the icons correctly in the menu, and you'll be able to run the application or open the corresponding file.

## Menu modes
- **ICON GRID** <br>
To make the created menu appear as a grid of icons, right-click on the corresponding shortcut, go to Properties, and at the end of the Target field (without deleting anything), leave a blank space and add:

`--iconmenu-NN`

NN should be replaced with a number indicating the number of columns to display. For example, to display a 3-column menu, type `--iconmenu-03` <br>

If you want the "Multimedia" folder to be displayed as a grid of icons, the Target field should look something like this:

`"C:\STACKY PLUS\stacky-plus.exe" "C:\STACKY PLUS\Multimedia" --iconmenu-03`

If you want the grid menu to have named icons, use:

`--iconmenu-NN-name`

NN should be replaced with a number indicating the number of columns to display.
In this menu mode, submenus and separators are not visible. Only the main menu shortcuts are shown.

<img width="572" height="239" alt="z11c- EscritorioW-Cuadríc-Cuadríc-name" src="https://github.com/user-attachments/assets/dfa3f4d1-26cd-4364-a989-fa0de45a0863" />

- **DOUBLE-COLUMN ICON MENU WITH SUBMENU** <br>
To make the created menu appear as a 2-column grid of icons with submenus, right-click on the corresponding shortcut, go to Properties, and at the end of the Target field (without deleting anything), leave a blank space and add:

`--iconmenu-C2`

If you want the menu to have named icons, use:

`--iconmenu-C2-name`

For example, if you want the "Multimedia" folder to be displayed as a double column of icons with submenus, the Target field should look similar to this:

`"C:\STACKY PLUS\stacky-plus.exe" "C:\STACKY PLUS\Multimedia" --iconmenu-C2`

In this menu mode, the separators are not visible.

<img width="572" height="384" alt="z13- EscritorioW-2C·y·2C·name" src="https://github.com/user-attachments/assets/ed2b14be-13e9-4b72-92e1-95ea732cfa42" />

- **DOUBLE-ROW ICON MENU WITH SUBMENU** <br>
To make the created menu appear as a grid of two rows of icons with submenus, right-click on the corresponding shortcut, go to Properties, and at the end of the Target field (without deleting anything), leave a blank space and add:

`--iconmenu-F2`

If you want the menu to have named icons, you must use:

`--iconmenu-F2-name`

<img width="572" height="332" alt="z14- EscritorioW-2F·y·2F-name" src="https://github.com/user-attachments/assets/9a513bac-dd7e-4ce7-b7ff-7f58b453b19d" />

## DARK MODE <br>
To display a menu in dark mode, right-click on the corresponding shortcut, go to Properties, and at the end of the Target field (without deleting anything), leave a blank space and add:

`--dark-mode`

All menu types support dark mode. For example, if you want the "Multimedia" folder to be displayed as a dark mode menu, the Target field should look something like this:

- `"C:\STACKY PLUS\stacky-plus.exe" "C:\STACKY PLUS\Multimedia" --dark-mode` for the default *menu with submenu* mode

- `"C:\STACKY PLUS\stacky-plus.exe" "C:\STACKY PLUS\Multimedia" --iconmenu-C2 --dark-mode` for *double column of icons with submenu* mode

and the same applies to other menu types.

<img width="730" height="374" alt="z16aa- EscritorioW-Modo oscuro" src="https://github.com/user-attachments/assets/73954ecf-2c1d-4ffa-ba9f-31349ef719f1" />

<img width="731" height="334" alt="z16bb- EscritorioW-Modo oscuro" src="https://github.com/user-attachments/assets/f86d0457-e032-4148-92cc-65281c6375ba" />

## MENU MODIFICATIONS
- **WITHOUT UNPINNING THE TASKBAR BUTTON**. You can add or remove menu or submenu shortcuts, add or remove submenu folders, separators, etc., without unpinning the taskbar button. After any of these changes, clicking the taskbar button will open the menu without icons. Clicking the button again will recreate the corresponding icons.
- **UNPINNING THE TASKBAR BUTTON**. The following modifications require removing the taskbar button (unpinning it) and recreating it:
If you change the menu mode. For example, from "Menu and Submenu" mode to "Icon Grid" mode, etc.
    - If you add or remove dark mode.
    - If you change the number of columns in Icon Grid mode.
    - If you change the menu button icon on the taskbar.
    - If the feature to open menu at mouse position is added or removed.
