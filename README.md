# Stacky-Plus, 5 menu modes for the Windows taskbar  

### Table of contents
- [Description](#description)
- [How many menus can I create on the taskbar?](#how-many-menus-can-i-create-on-the-taskbar)
- [Download](#download)
- [Why is it so fast?](#why-is-it-so-fast)
- [Steps to follow](#steps-to-follow)
- [Arrangement of elements](#arrangement-of-elements)
- [Other options](#other-options)
- [Shortcuts to Web Pages](#shortcuts-to-web-pages)
- [Context menu](#context-menu)
- [Menu Modifications](#menu-modifications)
- [Original Source](#original-source)
-----------------------------------------------

## Description 
There are 5 possible menu modes:<br>
#### **1) LIST WITH SUBMENUS** (default). Displays a list of shortcuts and multi-level submenus.
<img width="572" height="486" alt="00a- EscritorioW-Submenú" src="https://github.com/user-attachments/assets/731258f9-303e-44b7-8184-524778f3fd5f" /><br>
#### **2) ICON GRID**. Displays a single list of icons with a custom number of columns.<br>
<img width="747" height="239" alt="02 MODO-Cuadrícula de íconos" src="https://github.com/user-attachments/assets/79e60f5c-2a8a-4b9b-b854-99f9e1bf006c" /><br>
#### **3) DOUBLE COLUMN**. Both the main menu and submenus feature two columns, opening to the right or left.<br>
<img width="572" height="384" alt="03 MODO-Doble columna" src="https://github.com/user-attachments/assets/61c44a41-43f3-4f6d-9503-a05773b75339" /><br>
#### **4) DOUBLE ROW**. Both the main menu and submenus feature two rows, opening upwards.<br>
<img width="572" height="332" alt="04 MODO Doble fila" src="https://github.com/user-attachments/assets/4c282be6-3e0b-4e2e-906f-92c47157e854" /><br>
#### **5) SINGLE SUBMENU**. The main menu is a list, while each first-level submenu can be either a list or an icon grid with a custom number of columns.<br>
<img width="653" height="426" alt="05 MODO Submenú único" src="https://github.com/user-attachments/assets/e5959696-a421-4fe7-b165-be0c5c4b1855" />

## **How many menus can I create on the taskbar?**
As many as you want, and in any of the modes. Each one will have its own dedicated button on the taskbar.

## Download
The stacky-plus.exe file is 573 kB in size and can be downloaded from https://github.com/dieg467/Stacky-Plus/releases/tag/V-3.0
This application downloads as an .exe file, but it does not require installation.

## Why is it so fast?
Stacky-Plus stores the icons for all shortcuts in a small file within the corresponding folder. When the user opens a menu, Stacky-Plus accesses its cache immediately, without delay, at full speed.

## STEPS TO FOLLOW
The following steps explain how to create a menu on the taskbar.

**1) CREATING THE CONTAINER FOLDER**. Create a folder anywhere on your computer (on drive C, in My Documents, etc.), and name it STACKY PLUS, for example. Place the stacky-plus.exe file there.

**2) CREATION OF MENUS AND SUBMENUS** <br>
Inside the STACKY PLUS folder, create a **folder** to hold shortcuts. For example, you can create a "Multimedia" folder if you're going to place shortcuts (applications, folders, files, websites, etc.) related to music, videos, etc., there.<br>

<img width="655" height="304" alt="06-Crear carpeta Multimedia" src="https://github.com/user-attachments/assets/765f88d9-9259-452e-8482-3242e7c10968" /><br>

This folder will later become a menu in the taskbar. If you create several folders, such as "Multimedia", "Documents", "System", etc., each one will become a separate menu in the taskbar, each with its own button. <br>
Only folders located in the same directory as stacky-plus.exe can be converted into a menu. <br>
If you want a menu to have **submenus**, you must create subfolders within the "Multimedia," "Documents," etc., folders. You can create multiple levels of submenus by creating subfolders within subfolders. <br>
In the folder or subfolder where you want to create a **separator** (horizontal dividing line), create a .txt text file, give it a name, and change the .txt extension to the following:<br>

`.separator` <br>

You can create multiple separators within the same menu or submenu. The position of a separator depends on its name, as they are ordered alphabetically.<br>

<img width="655" height="423" alt="06 Separador" src="https://github.com/user-attachments/assets/bee9059d-1922-4273-bb4d-46c357be9da1" />

**3) CREATING THE MENU** <br>
Double-click the Stacky-plus.exe file. A CONFIGURATION window with two panels will open. The left panel will display the tree of folders and subfolders located within the Stacky plus container folder.<br>

<img width="746" height="613" alt="10 CONFIGURACION" src="https://github.com/user-attachments/assets/5b050c31-a1b8-411c-8f3c-4397be8c4f61" />

In the right panel you can select the menu name, its icon, whether to use small icons within the menu, the menu color, the opening position, the menu mode, the item order, and so on. <br>
Once you have selected all the menu settings, click "Create menu". A shortcut bearing the assigned menu name will then be created in the "Stacky plus" containing folder. Double-clicking the shortcut opens the created menu.<br>

<img width="677" height="522" alt="08-Menú·abre·en·carpeta" src="https://github.com/user-attachments/assets/eb7a90c2-1266-4189-971b-f386bd9aa9ce" /><br>
Dragging that shortcut to the taskbar creates the menu button.<br>

<img width="201" height="347" alt="07 Botón de menú" src="https://github.com/user-attachments/assets/6b135128-c557-4e15-8e53-0e1cf1a8ebba" />

## **Arrangement of elements**
- **Predetermined**<br>
By default, items (folder, subfolder, shortcut, separator) are sorted alphabetically.<br>
- **Folders first**<br>
If you select this option in the Settings window, the menu will display submenus first, followed by direct links. You can choose to separate these groups using an automatic separator. <br>
- **Personalized** <br>
If you want to customize the order of the items, the first item must begin with the prefix %01%, followed by its name. There is no need to leave a space between %01% and the item name. The %01% prefix will not be displayed in the menu. <br>
Subsequent items will be named %02%, %03%, etc. If an item does not have a prefix, it will be sorted alphabetically. <br>

<img width="752" height="423" alt="11- Ordenamiento" src="https://github.com/user-attachments/assets/3041343b-06a5-4d8b-b06e-0b2b98f762a2" />

## Other options
### Small icons<br>
You can select the small icon (mini) mode in the settings.<br>
- If this option is selected in the main menu, it will apply to all submenus.<br>
- If this option is selected in a submenu, it will apply to that submenu and its child submenus.<br>
<img width="414" height="305" alt="12 Mini" src="https://github.com/user-attachments/assets/f901802a-a27d-4902-8c10-3a6dc1766a06" /><br>
### **Color modes**<br>
There are three color modes: system (default), light, and dark.<br>
### **Menu at pointer position**<br>
Selecting this option in the main menu causes the menu to open at the mouse position, provided you have configured a keyboard shortcut.<br>
  
## Shortcuts to Web Pages
By default, website shortcuts appear with the browser icon (Chrome, Edge, Firefox, etc.). Once a website shortcut is in a menu, access the site via that shortcut so that Stacky-plus captures the website's icon (favicon) and adds it to the menu. Most icons are supported. <br>

<img width="373" height="140" alt="Pág-web" src="https://github.com/user-attachments/assets/4cb73a66-0933-4bdc-ac03-251e301f25c6" />

If Stacky-plus did not capture the icon/favicon, it can be customized via the corresponding shortcut. <br>

## Context menu
Right-clicking an item (shortcut or submenu) displays a context menu with options.

## Menu modifications
All modifications to a menu or submenu can be made from the Configuration window, either before or after it has been created. Changes are saved immediately when you switch folders or subfolders in the left panel, or whenever you press the Save button.

## Original Source
This project is based on Pawel Turlejski's original project (https://github.com/pawelt/stacky), which is no longer maintained, and
Clau-Bucur's fork (https://github.com/clau-bucur/stacky)
