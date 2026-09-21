! MILA driving-dataset builder.
!
! Turns telemetry exports (EXPORT CSV in the C++ / C# / F# controllers) into a training set for
! teaching MILA to drive herself by imitating a human: each row pairs what the ultrasonic sensor
! saw with the drive action the human was executing.
!
!   features:  front distance (+ how it is changing), speed, drive mode
!   label:     what the tracks were doing, as (motor_l, motor_r) in {-1,0,+1} and a class name/id
!
! WHAT IS KEPT
!   Only human-driven rows: modes "wasd" and "tank". The label comes from the actual motor state the
!   Arduino reports, so it is the same whether the human used the IR remote, the keyboard, a gamepad
!   or the dashboard. Rows are dropped when
!     - the mode is "obstacle": the robot was driving itself, that is not a human demonstration
!     - the collision guard is active: the guard overrode the human's command
!   Left/right ultrasonic readings are deliberately NOT features: the firmware only measures them
!   during the obstacle-mode scan, so in human-driven data they are stale.
!
! LEGACY EXPORTS (no motor columns) fall back to the last web command, which cannot see IR driving.
!
! BUILD
!   gfortran -O2 -Wall -Wextra -std=f2008 telemetry_common.f90 make_dataset.f90 -o make_dataset.exe
!
! USAGE
!   make_dataset.exe drive_policy.csv telemetry_a.csv telemetry_b.csv ...
!
! Each input file is one session: window features never cross a session boundary, and the last
! 20 % of every session (in time) is marked split=val, so validation data is never from the past
! of the training data.
!
! OUTPUT COLUMNS
!   session,t_s,split,mode,mode_id,dist,dist_valid,d_dist,dist_avg5,dist_min5,speed,
!   motor_l,motor_r,action,action_id,next_action,next_action_id
!
!   action_id: 0 STOP  1 FORWARD  2 BACKWARD  3 LEFT  4 RIGHT  5 L_FWD  6 L_BWD  7 R_FWD  8 R_BWD
!   dist_valid is 0 when the sensor returned no echo (0 cm); dist is then carried forward from the last
!   good reading. The sensor is polled about every 0.4 s, so `action` is what the human was doing at
!   that moment and `next_action` what they were doing one sample later (use whichever fits your model).

program make_dataset
  use, intrinsic :: iso_fortran_env, only: error_unit
  use telemetry_common
  implicit none

  integer, parameter :: window = 5
  integer, parameter :: maxfields = 16
  real(dp), parameter :: train_frac = 0.8_dp
  character(len=*), parameter :: header = &
       'session,t_s,split,mode,mode_id,dist,dist_valid,d_dist,dist_avg5,dist_min5,speed,' // &
       'motor_l,motor_r,action,action_id,next_action,next_action_id'
  character(len=9), parameter :: action_names(0:8) = [character(len=9) :: &
       'STOP', 'FORWARD', 'BACKWARD', 'LEFT', 'RIGHT', 'L_FWD', 'L_BWD', 'R_FWD', 'R_BWD']

  character(len=1024) :: outpath, path
  integer :: nargs, a, outu, ios, k
  integer :: rows_read, rows_kept, rows_train
  integer :: drop_obstacle, drop_guard, drop_other
  integer :: sessions_used, sessions_skipped, sessions_legacy
  integer :: class_count(0:8)

  nargs = command_argument_count()
  if (nargs < 2) then
    write (error_unit, '(A)') 'usage: make_dataset <out.csv> <telemetry.csv> [more.csv ...]'
    stop 2
  end if

  call get_command_argument(1, outpath)
  open (newunit=outu, file=trim(outpath), status='replace', action='write', iostat=ios)
  if (ios /= 0) then
    write (error_unit, '(2A)') 'cannot write ', trim(outpath)
    stop 1
  end if
  write (outu, '(A)') header

  rows_read = 0; rows_kept = 0; rows_train = 0
  drop_obstacle = 0; drop_guard = 0; drop_other = 0
  sessions_used = 0; sessions_skipped = 0; sessions_legacy = 0
  class_count = 0

  do a = 2, nargs
    call get_command_argument(a, path)
    call process_session(a - 1, trim(path))
  end do
  close (outu)

  ! ---- summary ----
  write (*, '(2A)') 'Driving dataset: ', trim(outpath)
  write (*, '(A,I0,A,I0,A)') 'Sessions:  ', sessions_used, ' used, ', sessions_skipped, ' skipped'
  write (*, '(A,I0)') 'Rows read: ', rows_read
  write (*, '(A,I0,A,I0,A,I0,A)') 'Rows kept: ', rows_kept, '  (train ', rows_train, ', val ', &
       rows_kept - rows_train, ')'
  write (*, '(A,I0,A)') '  dropped, robot was driving itself (obstacle mode): ', drop_obstacle, ' rows'
  write (*, '(A,I0,A)') '  dropped, collision guard overrode the human:        ', drop_guard, ' rows'
  if (drop_other > 0) write (*, '(A,I0,A)') '  dropped, unknown mode:                              ', drop_other, ' rows'
  if (sessions_legacy > 0) then
    write (*, '(A,I0,A)') 'NOTE: ', sessions_legacy, ' session(s) had no motor columns, so labels come from the '// &
         'last web command; IR-remote driving is not captured in those.'
  end if

  if (rows_kept == 0) then
    write (error_unit, '(A)') 'No usable rows: was the robot in wasd/tank mode and moving?'
    stop 1
  end if

  write (*, *)
  write (*, '(A)') 'Action distribution (kept rows):'
  do k = 0, 8
    if (class_count(k) > 0) then
      write (*, '(2X,I1,1X,A9,I8,A,A,A)') k, action_names(k), class_count(k), '  (', &
           fstr(100.0_dp * class_count(k) / rows_kept, 1), '%)'
    end if
  end do
  if (class_count(0) > rows_kept / 2) then
    write (*, '(A)') 'Heads up: STOP is over half the data. Weight the classes or drive more when you record.'
  end if

contains

  subroutine process_session(sid, path)
    integer, intent(in) :: sid
    character(len=*), intent(in) :: path

    character(len=1024) :: line
    character(len=64)   :: fields(maxfields), tmp
    character(len=16), allocatable :: modes(:), cmds(:)
    real(dp), allocatable :: t(:), dist(:), speed(:), avg(:), mn(:)
    logical,  allocatable :: dist_have(:), dvalid(:), speed_have(:), motor_have(:)
    integer,  allocatable :: guard(:), ml(:), mr(:)
    character(len=:), allocatable :: row, next_name, next_id, split
    integer  :: u, ios, nrows, n, nf, r, lo, ntrain, aid, aid2, mode_id, pl, pr, first_known
    real(dp) :: x, y, last
    logical  :: ok, ok2, any_motor

    open (newunit=u, file=path, status='old', action='read', iostat=ios)
    if (ios /= 0) then
      write (error_unit, '(2A)') 'cannot open ', path
      sessions_skipped = sessions_skipped + 1
      return
    end if

    ! pass 1: count rows
    nrows = 0
    read (u, '(A)', iostat=ios) line            ! header
    do
      read (u, '(A)', iostat=ios) line
      if (ios /= 0) exit
      call strip_cr(line)
      if (len_trim(line) > 0) nrows = nrows + 1
    end do
    if (nrows == 0) then
      close (u)
      sessions_skipped = sessions_skipped + 1
      return
    end if

    allocate (t(nrows), dist(nrows), speed(nrows), avg(nrows), mn(nrows))
    allocate (dist_have(nrows), dvalid(nrows), speed_have(nrows), motor_have(nrows))
    allocate (guard(nrows), ml(nrows), mr(nrows), modes(nrows), cmds(nrows))
    t = 0.0_dp; dist = 0.0_dp; speed = 100.0_dp
    dist_have = .false.; speed_have = .false.; motor_have = .false.
    guard = 0; ml = 0; mr = 0; modes = ''; cmds = ''

    ! pass 2: parse
    rewind (u)
    read (u, '(A)', iostat=ios) line
    n = 0
    do
      read (u, '(A)', iostat=ios) line
      if (ios /= 0) exit
      call strip_cr(line)
      if (len_trim(line) == 0) cycle
      n = n + 1
      call split_csv(line, fields, nf)
      t(n) = epoch_seconds(fields(1))
      call parse_real(fields(2), x, ok)
      if (ok) then
        dist(n) = x
        dist_have(n) = .true.
      end if
      call parse_real(fields(7), x, ok)
      if (ok) then
        speed(n) = x
        speed_have(n) = .true.
      end if
      tmp = adjustl(fields(8))
      modes(n) = tmp(1:len(modes))
      tmp = adjustl(fields(9))
      cmds(n) = tmp(1:len(cmds))
      call parse_real(fields(10), x, ok)
      if (ok .and. x > 0.5_dp) guard(n) = 1
      call parse_real(fields(11), x, ok)
      call parse_real(fields(12), y, ok2)
      if (ok .and. ok2) then
        ml(n) = nint(x)
        mr(n) = nint(y)
        motor_have(n) = .true.
      end if
    end do
    close (u)

    ! "valid" = the sensor actually returned an echo (blank = nothing reported, 0 cm = no echo)
    do r = 1, nrows
      dvalid(r) = dist_have(r) .and. dist(r) > 0.0_dp
    end do

    ! a session needs at least one valid front-distance reading
    first_known = 0
    do r = 1, nrows
      if (dvalid(r)) then
        first_known = r
        exit
      end if
    end do
    if (first_known == 0) then
      write (error_unit, '(2A)') 'no valid distance readings in ', path
      sessions_skipped = sessions_skipped + 1
      return
    end if
    sessions_used = sessions_used + 1

    ! carry the last valid reading over gaps and no-echo zeros (and the first one back over leading gaps),
    ! so `dist` is never a fake "0 cm = touching the sensor"; dist_valid keeps the fact that it was carried
    last = dist(first_known)
    do r = 1, nrows
      if (dvalid(r)) then
        last = dist(r)
      else
        dist(r) = last
      end if
    end do
    last = 100.0_dp
    do r = 1, nrows
      if (speed_have(r)) then
        last = speed(r)
        exit
      end if
    end do
    do r = 1, nrows
      if (speed_have(r)) then
        last = speed(r)
      else
        speed(r) = last
      end if
    end do

    ! the label: actual motor state if the export has it, else derive it from the last web command
    any_motor = any(motor_have)
    pl = 0
    pr = 0
    if (any_motor) then
      do r = 1, nrows
        if (motor_have(r)) then
          pl = ml(r)
          pr = mr(r)
        else
          ml(r) = pl
          mr(r) = pr
        end if
      end do
    else
      sessions_legacy = sessions_legacy + 1
      do r = 1, nrows
        call cmd_to_motor(cmds(r), pl, pr)
        ml(r) = pl
        mr(r) = pr
      end do
    end if

    ! trailing-window features over the whole session (so filtered rows still count as history)
    do r = 1, nrows
      lo = max(1, r - window + 1)
      avg(r) = sum(dist(lo:r)) / real(r - lo + 1, dp)
      mn(r) = minval(dist(lo:r))
    end do

    ntrain = max(1, nint(train_frac * real(nrows, dp)))

    do r = 1, nrows
      rows_read = rows_read + 1

      select case (trim(modes(r)))
      case ('wasd')
        mode_id = 0
      case ('tank')
        mode_id = 1
      case ('obstacle')
        drop_obstacle = drop_obstacle + 1
        cycle
      case default
        drop_other = drop_other + 1
        cycle
      end select
      if (guard(r) == 1) then
        drop_guard = drop_guard + 1
        cycle
      end if

      aid = action_of(ml(r), mr(r))
      if (r < nrows) then
        aid2 = action_of(ml(r + 1), mr(r + 1))
        next_name = trim(action_names(aid2))
        next_id = itoa(aid2)
      else
        next_name = ''
        next_id = ''
      end if
      if (r <= ntrain) then
        split = 'train'
        rows_train = rows_train + 1
      else
        split = 'val'
      end if

      row = itoa(sid) // ',' // fstr(t(r) - t(1), 3) // ',' // split // ',' // trim(modes(r)) // ',' // &
            itoa(mode_id) // ',' // fstr(dist(r), 2) // ',' // itoa(merge(1, 0, dvalid(r))) // ',' // &
            fstr(merge(dist(r) - dist(max(1, r - 1)), 0.0_dp, r > 1), 2) // ',' // &
            fstr(avg(r), 2) // ',' // fstr(mn(r), 2) // ',' // itoa(nint(speed(r))) // ',' // &
            itoa(ml(r)) // ',' // itoa(mr(r)) // ',' // trim(action_names(aid)) // ',' // itoa(aid) // ',' // &
            next_name // ',' // next_id
      write (outu, '(A)') row

      rows_kept = rows_kept + 1
      class_count(aid) = class_count(aid) + 1
    end do
  end subroutine process_session

  ! (left, right) track state -> class id. Covers all nine combinations of {-1, 0, +1}.
  pure function action_of(l, r) result(id)
    integer, intent(in) :: l, r
    integer :: id
    select case (3 * (max(-1, min(1, l)) + 1) + (max(-1, min(1, r)) + 1))
    case (4);  id = 0   ! ( 0, 0) STOP
    case (8);  id = 1   ! (+1,+1) FORWARD
    case (0);  id = 2   ! (-1,-1) BACKWARD
    case (2);  id = 3   ! (-1,+1) LEFT   (spin left)
    case (6);  id = 4   ! (+1,-1) RIGHT  (spin right)
    case (7);  id = 5   ! (+1, 0) L_FWD
    case (1);  id = 6   ! (-1, 0) L_BWD
    case (5);  id = 7   ! ( 0,+1) R_FWD
    case (3);  id = 8   ! ( 0,-1) R_BWD
    case default; id = 0
    end select
  end function action_of

  ! Legacy exports only: the last web command -> track state (unknown commands keep the previous state).
  subroutine cmd_to_motor(cmd, l, r)
    character(len=*), intent(in) :: cmd
    integer, intent(inout) :: l, r
    select case (trim(cmd))
    case ('STOP');     l = 0;  r = 0
    case ('FORWARD');  l = 1;  r = 1
    case ('BACKWARD'); l = -1; r = -1
    case ('LEFT');     l = -1; r = 1
    case ('RIGHT');    l = 1;  r = -1
    case ('L_FWD');    l = 1;  r = 0
    case ('L_BWD');    l = -1; r = 0
    case ('R_FWD');    l = 0;  r = 1
    case ('R_BWD');    l = 0;  r = -1
    case default
      continue
    end select
  end subroutine cmd_to_motor

  function itoa(i) result(s)
    integer, intent(in) :: i
    character(len=:), allocatable :: s
    character(len=16) :: buf
    write (buf, '(I0)') i
    s = trim(buf)
  end function itoa

end program make_dataset
